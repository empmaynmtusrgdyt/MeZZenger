#include <QCoreApplication>
#include <QDebug>
#include <QCommandLineParser>
#include "ServerCore.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    
    QCommandLineParser parser;
    parser.setApplicationDescription("Сервер мессенджера");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption portOption(QStringList() << "p" << "port",
                                  "Порт для прослушивания", "port", "5555");
    parser.addOption(portOption);
    
    parser.process(app);
    
    quint16 port = parser.value(portOption).toUShort();
    if (port == 0) {
        port = 5555;
    }
    
    ServerCore server;
    if (!server.start(port)) {
        qCritical() << "Не удалось запустить сервер";
        return 1;
    }
    
    qDebug().noquote() << "Сервер работает. Нажмите Ctrl+C для остановки.";
    
    return app.exec();
}
