#ifndef SERVER_CORE_H
#define SERVER_CORE_H

#include <QObject>
#include <QSslServer>
#include <QTcpSocket>
#include <QMap>
#include <QList>
#include <QJsonObject>
#include "DatabaseServer.h"
#include "ClientHandler.h"

class ServerCore : public QObject {
    Q_OBJECT

public:
    explicit ServerCore(QObject *parent = nullptr);
    ~ServerCore();

    bool start(quint16 port = 5555, const QString &certPath = "server.crt",
               const QString &keyPath = "server.key");
    void stop();

    void sendMessageToUser(int userId, int messageId, const QJsonObject &message);

private:
    QSslServer m_server;
    DatabaseServer m_database;
    QMap<int, ClientHandler*> m_clients;

    void onNewConnection();
};

#endif
