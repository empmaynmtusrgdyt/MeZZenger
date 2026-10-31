#include "TlsTrust.h"
#include <QCryptographicHash>
#include <QMessageBox>
#include <QSettings>
#include <QDebug>

namespace TlsTrust {

QString fingerprint(const QSslCertificate &certificate) {
    return QString::fromLatin1(
        QCryptographicHash::hash(certificate.toDer(), QCryptographicHash::Sha256).toHex(':').toUpper());
}

void attach(QSslSocket *socket, const QString &host, quint16 port, std::function<QWidget *()> dialogParent,
            std::function<void()> onRejected) {
    // ignoreSslErrors() запоминается сокетом: без сброса при переподключении
    // sslErrors не приходит и отпечаток больше не сверяется.
    QObject::connect(socket, &QAbstractSocket::stateChanged, socket,
        [socket](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::HostLookupState) {
                socket->ignoreSslErrors(QList<QSslError>());
            }
        });

    QObject::connect(socket, &QSslSocket::sslErrors, socket,
        [socket, host, port, dialogParent, onRejected](const QList<QSslError> &errors) {
            const QSslCertificate certificate = socket->peerCertificate();
            if (certificate.isNull()) {
                return;
            }

            const QString current = fingerprint(certificate);
            const QString settingsKey = QString("trusted/%1:%2").arg(host).arg(port);
            QSettings settings;
            const QString pinned = settings.value(settingsKey).toString();

            if (pinned == current) {
                socket->ignoreSslErrors(errors);
                return;
            }

            QString question;
            QMessageBox::Icon icon;
            if (pinned.isEmpty()) {
                icon = QMessageBox::Question;
                question = QString("Вы подключаетесь к серверу %1:%2 впервые.\n\n"
                                   "Отпечаток сертификата (SHA-256):\n%3\n\n"
                                   "Сверьте его с отпечатком, который показал администратор сервера. "
                                   "Доверять этому серверу?").arg(host).arg(port).arg(current);
            } else {
                icon = QMessageBox::Warning;
                question = QString("ВНИМАНИЕ: сертификат сервера %1:%2 изменился!\n\n"
                                   "Было: %3\nСтало: %4\n\n"
                                   "Это может означать подмену сервера. Продолжайте, только если "
                                   "администратор сообщил о смене сертификата. Доверять новому?")
                               .arg(host).arg(port).arg(pinned, current);
            }

            QMessageBox box(icon, "Сертификат сервера", question,
                            QMessageBox::Yes | QMessageBox::No, dialogParent ? dialogParent() : nullptr);
            box.setDefaultButton(QMessageBox::No);
            if (box.exec() == QMessageBox::Yes) {
                settings.setValue(settingsKey, current);
                socket->ignoreSslErrors(errors);
            } else {
                qDebug() << "Пользователь отказался доверять сертификату" << host << port;
                if (onRejected) {
                    onRejected();
                }
            }
        });
}

}
