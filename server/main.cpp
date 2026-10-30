#include <QCoreApplication>
#include <QDebug>
#include <QCommandLineParser>
#include <QTextStream>
#include "ServerCore.h"
#include "DatabaseServer.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    
    QCommandLineParser parser;
    parser.setApplicationDescription("Сервер MeZZenger");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption portOption(QStringList() << "p" << "port",
                                  "Порт для прослушивания", "port", "5555");
    parser.addOption(portOption);

    QCommandLineOption certOption("cert", "Файл сертификата TLS (PEM)", "file", "server.crt");
    QCommandLineOption keyOption("key", "Файл закрытого ключа TLS (PEM)", "file", "server.key");
    parser.addOption(certOption);
    parser.addOption(keyOption);

    QCommandLineOption inviteOption("new-invite", "Создать одноразовый код приглашения для регистрации и выйти");
    parser.addOption(inviteOption);
    
    parser.process(app);
    
    if (parser.isSet(inviteOption)) {
        DatabaseServer database;
        if (!database.initialize("server.db")) {
            return 1;
        }
        const QString code = database.createInvite();
        if (code.isEmpty()) {
            return 1;
        }
        QTextStream(stdout) << code << Qt::endl;
        return 0;
    }

    quint16 port = parser.value(portOption).toUShort();
    if (port == 0) {
        port = 5555;
    }
    
    ServerCore server;
    if (!server.start(port, parser.value(certOption), parser.value(keyOption))) {
        qCritical() << "Не удалось запустить сервер";
        return 1;
    }
    
    qDebug().noquote() << "Сервер работает. Нажмите Ctrl+C для остановки.";
    
    return app.exec();
}
