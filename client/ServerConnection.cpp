#include "ServerConnection.h"
#include "Protocol.h"
#include "TlsTrust.h"
#include <QDebug>

ServerConnection::ServerConnection(QObject *parent)
    : QObject(parent)
    , m_socket(new QSslSocket(this))
    , m_reconnectTimer(new QTimer(this))
    , m_attemptTimer(new QTimer(this))
{
    m_reconnectTimer->setSingleShot(true);
    m_attemptTimer->setSingleShot(true);

    connect(m_socket, &QSslSocket::readyRead, this, &ServerConnection::onReadyRead);
    connect(m_socket, &QSslSocket::encrypted, this, &ServerConnection::onEncrypted);
    connect(m_socket, &QSslSocket::disconnected, this, &ServerConnection::onDisconnected);
    connect(m_socket, &QSslSocket::errorOccurred, this, &ServerConnection::onError);
    // Пока пользователь читает отпечаток сертификата, таймаут не должен сработать
    connect(m_socket, &QSslSocket::sslErrors, m_attemptTimer, &QTimer::stop);

    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "Reconnect timeout, attempting to reconnect";
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            connectNow();
        }
    });
    connect(m_attemptTimer, &QTimer::timeout, this, [this]() {
        m_socket->abort();
        finishAttempt("Превышено время подключения");
    });
}

ServerConnection::~ServerConnection() {
    m_socket->disconnect(this);
    m_socket->abort();
}

void ServerConnection::open(const QString &host, quint16 port, const QString &nick,
                            const QString &password, const QString &invite) {
    m_host = host;
    m_port = port;
    m_nick = nick;
    m_password = password;
    m_invite = invite;
    m_rejected = false;
    m_attemptActive = true;

    TlsTrust::attach(m_socket, m_host, m_port, [this]() { return m_dialogParent.data(); }, [this]() {
        m_rejected = true;
        m_reconnectTimer->stop();
        m_attemptTimer->stop();
        emit tlsRejected();
        finishAttempt("Сертификат сервера не принят");
    });

    m_attemptTimer->start(5000);
    connectNow();
}

void ServerConnection::close() {
    m_autoReconnect = false;
    m_reconnectTimer->stop();
    m_attemptTimer->stop();
    m_socket->abort();
}

void ServerConnection::setAutoReconnect(bool enabled) {
    m_autoReconnect = enabled;
    if (enabled && !m_rejected && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_reconnectTimer->start(3000);
    }
}

void ServerConnection::startDispatch() {
    m_dispatching = true;
    while (!m_held.isEmpty()) {
        emit messageReceived(m_held.takeFirst());
    }
}

void ServerConnection::send(const QJsonObject &message) {
    m_socket->write(serializeMessage(message));
}

void ServerConnection::connectNow() {
    m_buffer.clear();
    m_socket->connectToHostEncrypted(m_host, m_port);
}

void ServerConnection::onEncrypted() {
    m_attemptTimer->stop();
    qDebug() << "Socket connected, sending auth request for" << m_nick;
    if (m_invite.isEmpty()) {
        send(ProtocolHelper::createLoginMessage(m_nick, m_password));
    } else {
        send(ProtocolHelper::createRegisterMessage(m_nick, m_password, m_invite));
    }
}

void ServerConnection::onReadyRead() {
    m_buffer += m_socket->readAll();

    while (m_buffer.size() >= static_cast<int>(HeaderSize)) {
        const uint32_t length = readHeader(m_buffer.left(HeaderSize));
        if (length == 0 || length > MaxMessageSize) {
            qDebug() << "Invalid message length";
            m_buffer.clear();
            m_socket->abort();
            onDisconnected();
            return;
        }
        if (m_buffer.size() < static_cast<int>(HeaderSize + length)) {
            break;
        }

        const QJsonObject message = deserializeMessage(m_buffer.mid(HeaderSize, length));
        m_buffer = m_buffer.mid(HeaderSize + length);
        if (message.isEmpty()) {
            continue;
        }

        if (ProtocolHelper::parseMessageType(message) == Protocol::MessageType::AuthResponse) {
            handleAuthResponse(message);
        } else if (m_dispatching) {
            emit messageReceived(message);
        } else {
            m_held.append(message);
        }
    }
}

void ServerConnection::handleAuthResponse(const QJsonObject &message) {
    const bool success = ProtocolHelper::getBool(message, Protocol::SUCCESS);
    const QString text = ProtocolHelper::getString(message, Protocol::MESSAGE);
    qDebug() << "Auth response success:" << success << "msg:" << text;

    if (success) {
        // Регистрация выполнена один раз, дальше только вход
        m_invite.clear();
        m_authenticated = true;
        m_attemptActive = false;
        emit authenticated();
        return;
    }

    m_autoReconnect = false;
    m_reconnectTimer->stop();
    m_attemptActive = false;
    m_socket->abort();
    emit authRejected(text);
}

void ServerConnection::finishAttempt(const QString &error) {
    if (!m_attemptActive) {
        return;
    }
    m_attemptActive = false;
    m_socket->abort();
    emit failed(error);
}

void ServerConnection::onDisconnected() {
    qDebug() << "Disconnected from server";
    const bool wasAuthenticated = m_authenticated;
    m_authenticated = false;
    if (wasAuthenticated || m_autoReconnect) {
        emit disconnected();
    }
    if (m_autoReconnect && !m_rejected) {
        m_reconnectTimer->start(3000);
    }
}

void ServerConnection::onError() {
    qDebug() << "Socket error:" << m_socket->errorString();
    if (m_rejected) {
        return;
    }
    m_attemptTimer->stop();
    if (m_attemptActive) {
        finishAttempt("Ошибка соединения: " + m_socket->errorString());
        return;
    }
    // Неудачная попытка подключения не даёт сигнала disconnected, повторяем сами
    if (m_autoReconnect && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_reconnectTimer->start(3000);
    }
}
