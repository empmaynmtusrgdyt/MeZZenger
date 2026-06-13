#include "NetworkManager.h"
#include "Protocol.h"
#include <QDebug>

NetworkManager::NetworkManager(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &NetworkManager::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &NetworkManager::onDisconnected);
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
            this, &NetworkManager::onError);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkManager::readMessages);
}

NetworkManager::~NetworkManager() {
    disconnectFromHost();
}

bool NetworkManager::connectToHost(const QString &host, quint16 port) {
    m_host = host;
    m_port = port;
    m_socket->connectToHost(host, port);
    return true;
}

void NetworkManager::disconnectFromHost() {
    m_socket->disconnectFromHost();
    m_socket->close();
}

bool NetworkManager::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void NetworkManager::sendMessage(const QJsonObject &message) {
    if (!isConnected()) {
        qWarning() << "Невозможно отправить сообщение: не подключены к серверу";
        return;
    }
    
    QByteArray data = serializeMessage(message);
    qint64 sent = m_socket->write(data);
    if (sent < 0) {
        qWarning() << "Ошибка отправки сообщения:" << m_socket->errorString();
    }
}

void NetworkManager::onConnected() {
    qDebug() << "Подключено к серверу:" << m_host << ":" << m_port;
    emit connected();
}

void NetworkManager::onDisconnected() {
    qDebug() << "Отключены от сервера";
    emit disconnected();
}

void NetworkManager::onError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    QString errorMsg = m_socket->errorString();
    qWarning() << "Ошибка сети:" << errorMsg;
    emit errorOccurred(errorMsg);
}

void NetworkManager::readMessages() {
    m_buffer += m_socket->readAll();
    
    while (m_buffer.size() >= static_cast<int>(HeaderSize)) {
        uint32_t messageLength = readHeader(m_buffer.left(HeaderSize));
        
        if (messageLength == 0 || messageLength > MaxMessageSize) {
            qWarning() << "Неверная длина сообщения";
            m_buffer.clear();
            m_socket->disconnectFromHost();
            return;
        }
        
        if (m_buffer.size() < static_cast<int>(HeaderSize + messageLength)) {
            break;
        }
        
        QByteArray messageData = m_buffer.mid(HeaderSize, messageLength);
        m_buffer = m_buffer.mid(HeaderSize + messageLength);
        
        QJsonObject message = deserializeMessage(messageData);
        if (message.isEmpty()) {
            qWarning() << "Ошибка парсинга JSON";
            continue;
        }
        
        emit messageReceived(message);
    }
}
