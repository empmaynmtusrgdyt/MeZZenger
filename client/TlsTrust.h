#ifndef TLS_TRUST_H
#define TLS_TRUST_H

#include <QSslCertificate>
#include <QSslSocket>
#include <QString>
#include <QWidget>
#include <functional>

// Доверие самоподписанному сертификату сервера по отпечатку (как known_hosts в SSH):
// при первом подключении пользователь подтверждает отпечаток, дальше он сверяется молча.
namespace TlsTrust {

QString fingerprint(const QSslCertificate &certificate);

// Подключает обработку ошибок сертификата к сокету. onRejected вызывается,
// если пользователь отказался доверять серверу.
void attach(QSslSocket *socket, const QString &host, quint16 port, QWidget *dialogParent,
            std::function<void()> onRejected = {});

}

#endif
