#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QJsonObject>

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
    void loginSuccessful();
    void loginFailed(const QString &error);

private slots:
    void onLoginClicked();
    void onRegisterClicked();

private:
    QLineEdit *m_hostEdit;
    QLineEdit *m_portEdit;
    QLineEdit *m_nickEdit;
    QLineEdit *m_passwordEdit;
    
    QPushButton *m_loginButton;
    QPushButton *m_registerButton;
    
    QLabel *m_statusLabel;
    
    bool m_isRegistering = false;
    QString m_currentHost;
    quint16 m_currentPort = 5555;

    void setupUI();
    void applyStyles();
    void sendAuthRequest(const QString &type);
};

#endif
