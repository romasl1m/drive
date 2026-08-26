#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>

#include "../core/backup_engine.h"

namespace drive {

class AuthWidget : public QWidget {
    Q_OBJECT
public:
    explicit AuthWidget(BackupEngine *engine, QWidget *parent = nullptr);

    void showWelcome();
    void showPhoneInput();
    void showCodeInput();
    void showQR();
    void showError(const std::string &msg);

signals:
    void authenticated();

private slots:
    void onPhoneSubmit();
    void onCodeSubmit();
    void onQRLogin();
    void pollQR();

private:
    BackupEngine *engine_;
    QStackedWidget *pages_;

    // Welcome page
    QWidget *welcome_page_;

    // Phone page
    QWidget *phone_page_;
    QLineEdit *phone_input_;
    QLabel *phone_error_;

    // Code page
    QWidget *code_page_;
    QLineEdit *code_input_;
    QLabel *code_error_;

    // QR page
    QWidget *qr_page_;
    QLabel *qr_image_;
    QTimer *qr_timer_;
};

} // namespace drive
