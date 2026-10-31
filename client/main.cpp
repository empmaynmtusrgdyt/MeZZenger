#include <QApplication>
#include <QStyleFactory>
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
    
    QObject::connect(&loginWindow, &LoginWindow::loginSuccessful, [&](ServerConnection *connection) {
        MainWindow *mainWindow = new MainWindow(connection, loginWindow.getNick());
        mainWindow->show();

        loginWindow.hide();

        QObject::connect(mainWindow, &MainWindow::destroyed, &loginWindow, &QObject::deleteLater);
    });

    return app.exec();
}
