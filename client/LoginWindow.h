#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QPointer>

class ServerConnection;

class LoginWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

    QString getHost() const;
    int getPort() const;
    QString getNick() const;
    QString getPassword() const;

signals:
    // Соединение, в котором прошёл вход, передаётся главному окну
    void loginSuccessful(ServerConnection *connection);

private slots:
    void onLoginClicked();
    void onRegisterClicked();

private:
    QLineEdit *m_hostEdit;
    QLineEdit *m_portEdit;
    QLineEdit *m_nickEdit;
    QLineEdit *m_passwordEdit;
    QLineEdit *m_inviteEdit;
    
    QPushButton *m_loginButton;
    QPushButton *m_registerButton;
    
    QLabel *m_statusLabel;
    
    QPointer<ServerConnection> m_connection;

    void setupUI();
    void applyStyles();
    void startAuth(const QString &type);
    void setBusy(bool busy);
};

#endif
