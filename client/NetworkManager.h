#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QThread>

class NetworkManager : public QObject {
    Q_OBJECT

public:
    explicit NetworkManager(QObject *parent = nullptr);
    ~NetworkManager();

    bool connectToHost(const QString &host, quint16 port);
    void disconnectFromHost();
    bool isConnected() const;

    void sendMessage(const QJsonObject &message);

    QString getHost() const { return m_host; }
    quint16 getPort() const { return m_port; }

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &error);
    void messageReceived(const QJsonObject &message);

public slots:
    void readMessages();

private:
    QTcpSocket *m_socket;
    QString m_host;
    quint16 m_port = 0;
    QByteArray m_buffer;

    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
};

#endif
