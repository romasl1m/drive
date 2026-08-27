#include "auth_widget.h"
#include <QPixmap>
#include <QPainter>
#include <QFont>
#include <QRegularExpression>
#include <qrencode.h>

namespace drive {

AuthWidget::AuthWidget(BackupEngine *engine, QWidget *parent)
    : QWidget(parent), engine_(engine) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(40, 40, 40, 40);

    pages_ = new QStackedWidget;
    layout->addWidget(pages_);

    // Shared status label at the bottom
    status_label_ = new QLabel;
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setStyleSheet("color: #6b7280; font-size: 12px; padding-top: 16px;");
    layout->addWidget(status_label_);

    // --- Welcome page ---
    welcome_page_ = new QWidget;
    auto *wl = new QVBoxLayout(welcome_page_);
    wl->setAlignment(Qt::AlignCenter);
    wl->setSpacing(16);

    auto *title = new QLabel("Drive");
    title->setAlignment(Qt::AlignCenter);
    auto title_font = title->font();
    title_font.setPointSize(28);
    title_font.setBold(true);
    title->setFont(title_font);
    wl->addWidget(title);

    auto *subtitle = new QLabel("Private unlimited backup\npowered by Telegram");
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet("color: #6b7280; font-size: 14px;");
    wl->addWidget(subtitle);

    wl->addSpacing(24);

    auto *phone_btn = new QPushButton("Sign in with phone number");
    phone_btn->setMinimumHeight(44);
    phone_btn->setMinimumWidth(280);
    phone_btn->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; padding: 12px 24px; }"
        "QPushButton:hover { background: #333; }");
    connect(phone_btn, &QPushButton::clicked, this, &AuthWidget::showPhoneInput);
    wl->addWidget(phone_btn, 0, Qt::AlignCenter);

    auto *qr_btn = new QPushButton("Sign in with QR code");
    qr_btn->setMinimumHeight(44);
    qr_btn->setMinimumWidth(280);
    qr_btn->setStyleSheet(
        "QPushButton { background: transparent; color: #1a1a1a; border: 1px solid #e5e7eb; border-radius: 8px; font-size: 14px; padding: 12px 24px; }"
        "QPushButton:hover { border-color: #1a1a1a; }");
    connect(qr_btn, &QPushButton::clicked, this, &AuthWidget::onQRLogin);
    wl->addWidget(qr_btn, 0, Qt::AlignCenter);

    pages_->addWidget(welcome_page_);

    // --- Phone page ---
    phone_page_ = new QWidget;
    auto *pl = new QVBoxLayout(phone_page_);
    pl->setAlignment(Qt::AlignCenter);
    pl->setSpacing(12);
    pl->setContentsMargins(0, 0, 0, 0);

    auto *phone_title = new QLabel("Phone number");
    phone_title->setStyleSheet("font-size: 18px; font-weight: 600;");
    pl->addWidget(phone_title);

    auto *phone_hint = new QLabel("Include your country code (e.g. +1 for US, +44 for UK)");
    phone_hint->setStyleSheet("color: #6b7280; font-size: 12px;");
    phone_hint->setWordWrap(true);
    pl->addWidget(phone_hint);

    phone_input_ = new QLineEdit;
    phone_input_->setPlaceholderText("+1 234 567 8900");
    phone_input_->setMinimumHeight(44);
    phone_input_->setMaximumWidth(320);
    phone_input_->setStyleSheet(
        "QLineEdit { border: 1px solid #e5e7eb; border-radius: 8px; padding: 10px 14px; font-size: 16px; font-family: monospace; }"
        "QLineEdit:focus { border-color: #2563eb; }");
    pl->addWidget(phone_input_);

    phone_error_ = new QLabel;
    phone_error_->setStyleSheet("color: #dc2626; font-size: 12px;");
    phone_error_->setWordWrap(true);
    phone_error_->hide();
    pl->addWidget(phone_error_);

    auto *phone_submit = new QPushButton("Continue");
    phone_submit->setMinimumHeight(44);
    phone_submit->setMaximumWidth(320);
    phone_submit->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; }"
        "QPushButton:hover { background: #333; }");
    connect(phone_submit, &QPushButton::clicked, this, &AuthWidget::onPhoneSubmit);
    connect(phone_input_, &QLineEdit::returnPressed, this, &AuthWidget::onPhoneSubmit);
    pl->addWidget(phone_submit);

    auto *phone_back = new QPushButton("Back");
    phone_back->setFlat(true);
    phone_back->setStyleSheet("color: #6b7280; font-size: 13px; padding: 8px;");
    connect(phone_back, &QPushButton::clicked, this, &AuthWidget::showWelcome);
    pl->addWidget(phone_back);

    pages_->addWidget(phone_page_);

    // --- Code page ---
    code_page_ = new QWidget;
    auto *cl = new QVBoxLayout(code_page_);
    cl->setAlignment(Qt::AlignCenter);
    cl->setSpacing(12);
    cl->setContentsMargins(0, 0, 0, 0);

    auto *code_title = new QLabel("Enter the code");
    code_title->setStyleSheet("font-size: 18px; font-weight: 600;");
    cl->addWidget(code_title);

    auto *code_hint = new QLabel("A code was sent to your Telegram app");
    code_hint->setStyleSheet("color: #6b7280; font-size: 13px;");
    cl->addWidget(code_hint);

    code_input_ = new QLineEdit;
    code_input_->setPlaceholderText("12345");
    code_input_->setMaxLength(6);
    code_input_->setMinimumHeight(44);
    code_input_->setMaximumWidth(200);
    code_input_->setStyleSheet(
        "QLineEdit { border: 1px solid #e5e7eb; border-radius: 8px; padding: 10px 14px; font-size: 22px; font-family: monospace; letter-spacing: 6px; }"
        "QLineEdit:focus { border-color: #2563eb; }");
    cl->addWidget(code_input_);

    code_error_ = new QLabel;
    code_error_->setStyleSheet("color: #dc2626; font-size: 12px;");
    code_error_->setWordWrap(true);
    code_error_->hide();
    cl->addWidget(code_error_);

    auto *code_submit = new QPushButton("Verify");
    code_submit->setMinimumHeight(44);
    code_submit->setMaximumWidth(200);
    code_submit->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; }"
        "QPushButton:hover { background: #333; }");
    connect(code_submit, &QPushButton::clicked, this, &AuthWidget::onCodeSubmit);
    connect(code_input_, &QLineEdit::returnPressed, this, &AuthWidget::onCodeSubmit);
    cl->addWidget(code_submit);

    auto *code_back = new QPushButton("Back");
    code_back->setFlat(true);
    code_back->setStyleSheet("color: #6b7280; font-size: 13px; padding: 8px;");
    connect(code_back, &QPushButton::clicked, this, &AuthWidget::showWelcome);
    cl->addWidget(code_back);

    pages_->addWidget(code_page_);

    // --- QR page ---
    qr_page_ = new QWidget;
    auto *ql = new QVBoxLayout(qr_page_);
    ql->setAlignment(Qt::AlignCenter);
    ql->setSpacing(16);

    auto *qr_title = new QLabel("Scan with Telegram");
    qr_title->setStyleSheet("font-size: 18px; font-weight: 600;");
    qr_title->setAlignment(Qt::AlignCenter);
    ql->addWidget(qr_title);

    qr_image_ = new QLabel;
    qr_image_->setFixedSize(220, 220);
    qr_image_->setAlignment(Qt::AlignCenter);
    qr_image_->setStyleSheet("background: white; border-radius: 12px; border: 1px solid #e5e7eb;");
    ql->addWidget(qr_image_, 0, Qt::AlignCenter);

    auto *qr_hint = new QLabel("Open Telegram > Settings >\nDevices > Link Desktop Device");
    qr_hint->setAlignment(Qt::AlignCenter);
    qr_hint->setStyleSheet("color: #6b7280; font-size: 13px;");
    ql->addWidget(qr_hint);

    auto *qr_back = new QPushButton("Back");
    qr_back->setFlat(true);
    qr_back->setStyleSheet("color: #6b7280; font-size: 13px; padding: 8px;");
    connect(qr_back, &QPushButton::clicked, this, &AuthWidget::showWelcome);
    ql->addWidget(qr_back);

    pages_->addWidget(qr_page_);

    // QR poll timer
    qr_timer_ = new QTimer(this);
    connect(qr_timer_, &QTimer::timeout, this, &AuthWidget::pollQR);

    showWelcome();
}

void AuthWidget::showWelcome() {
    qr_timer_->stop();
    status_label_->clear();
    pages_->setCurrentWidget(welcome_page_);
}

void AuthWidget::showPhoneInput() {
    qr_timer_->stop();
    phone_error_->hide();
    status_label_->clear();
    pages_->setCurrentWidget(phone_page_);
    phone_input_->setFocus();
}

void AuthWidget::showCodeInput() {
    qr_timer_->stop();
    code_error_->hide();
    code_input_->clear();
    status_label_->setText("Code sent to your Telegram app");
    pages_->setCurrentWidget(code_page_);
    code_input_->setFocus();
}

void AuthWidget::showQR() {
    pages_->setCurrentWidget(qr_page_);
    status_label_->setText("Waiting for QR scan...");
    if (!qr_timer_->isActive())
        qr_timer_->start(500);
    pollQR();
}

void AuthWidget::showError(const std::string &msg) {
    if (msg.empty()) return;
    QString err = QString::fromStdString(msg);
    status_label_->setText(err);
    status_label_->setStyleSheet("color: #dc2626; font-size: 12px; padding-top: 16px;");

    auto current = pages_->currentWidget();
    if (current == phone_page_) {
        phone_error_->setText(err);
        phone_error_->show();
    } else if (current == code_page_) {
        code_error_->setText(err);
        code_error_->show();
    }
}

void AuthWidget::onPhoneSubmit() {
    QString phone = phone_input_->text().trimmed();
    phone.remove(QRegularExpression("[^+\\d]"));
    if (phone.isEmpty() || phone.length() < 5) {
        phone_error_->setText("Please enter a valid phone number with country code");
        phone_error_->show();
        return;
    }
    phone_error_->hide();
    status_label_->setStyleSheet("color: #6b7280; font-size: 12px; padding-top: 16px;");
    status_label_->setText("Sending code to " + phone + "...");
    engine_->submit_phone(phone.toStdString());
}

void AuthWidget::onCodeSubmit() {
    QString code = code_input_->text().trimmed();
    if (code.isEmpty()) return;
    code_error_->hide();
    status_label_->setStyleSheet("color: #6b7280; font-size: 12px; padding-top: 16px;");
    status_label_->setText("Verifying...");
    engine_->submit_code(code.toStdString());
}

void AuthWidget::onQRLogin() {
    status_label_->setStyleSheet("color: #6b7280; font-size: 12px; padding-top: 16px;");
    status_label_->setText("Requesting QR code...");
    last_qr_link_.clear();
    engine_->request_qr_login();
    showQR();
}

void AuthWidget::pollQR() {
    std::string link = engine_->qr_link();
    if (link.empty()) return;

    QString qlink = QString::fromStdString(link);
    if (qlink == last_qr_link_) return;
    last_qr_link_ = qlink;

    status_label_->setText("Scan the QR code with Telegram");

    QRcode *qr = QRcode_encodeString(link.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) {
        status_label_->setText("Failed to generate QR code");
        return;
    }

    int size = 200;
    int modules = qr->width;
    int scale = size / modules;
    if (scale < 2) scale = 2;
    int img_size = scale * modules;
    int offset = (220 - img_size) / 2;

    QPixmap pix(220, 220);
    pix.fill(Qt::white);
    QPainter painter(&pix);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);

    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            if (qr->data[y * modules + x] & 1) {
                painter.drawRect(offset + x * scale, offset + y * scale, scale, scale);
            }
        }
    }
    painter.end();
    QRcode_free(qr);

    qr_image_->setPixmap(pix);
}

} // namespace drive
