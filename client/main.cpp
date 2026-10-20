#include <QApplication>
#include <QStyleFactory>
#include <QDebug>
#include "LoginWindow.h"
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    app.setApplicationName("MeZZenger");
    app.setOrganizationName("MeZZenger");
    app.setApplicationVersion("1.0.0");
    
    app.setStyle(QStyleFactory::create("Fusion"));
    
    LoginWindow loginWindow;
    loginWindow.show();
    
    QObject::connect(&loginWindow, &LoginWindow::loginSuccessful, [&]() {
        qDebug() << "loginSuccessful signal received";
        
        QString host = loginWindow.getHost();
        int port = loginWindow.getPort();
        QString nick = loginWindow.getNick();
        QString password = loginWindow.getPassword();
        
        qDebug() << "Creating MainWindow with host:" << host << "port:" << port << "nick:" << nick;
        
        MainWindow *mainWindow = new MainWindow(host, port, nick, password);
        mainWindow->show();
        
        loginWindow.hide();
        
        QObject::connect(mainWindow, &MainWindow::destroyed, &loginWindow, &QObject::deleteLater);
    });
    
    return app.exec();
}
