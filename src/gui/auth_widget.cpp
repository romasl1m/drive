#include "auth_widget.h"
#include <QPixmap>
#include <QPainter>
#include <QFont>
#include <QRegularExpression>

namespace drive {

AuthWidget::AuthWidget(BackupEngine *engine, QWidget *parent)
    : QWidget(parent), engine_(engine) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    pages_ = new QStackedWidget;
    layout->addWidget(pages_);

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
    phone_btn->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; padding: 12px 24px; }"
        "QPushButton:hover { background: #333; }");
    connect(phone_btn, &QPushButton::clicked, this, &AuthWidget::showPhoneInput);
    wl->addWidget(phone_btn);

    auto *qr_btn = new QPushButton("Sign in with QR code");
    qr_btn->setMinimumHeight(44);
    qr_btn->setStyleSheet(
        "QPushButton { background: transparent; color: #1a1a1a; border: 1px solid #e5e7eb; border-radius: 8px; font-size: 14px; padding: 12px 24px; }"
        "QPushButton:hover { border-color: #1a1a1a; }");
    connect(qr_btn, &QPushButton::clicked, this, &AuthWidget::onQRLogin);
    wl->addWidget(qr_btn);

    pages_->addWidget(welcome_page_);

    // --- Phone page ---
    phone_page_ = new QWidget;
    auto *pl = new QVBoxLayout(phone_page_);
    pl->setAlignment(Qt::AlignCenter);
    pl->setSpacing(12);

    auto *phone_title = new QLabel("Phone number");
    phone_title->setStyleSheet("font-size: 18px; font-weight: 600;");
    pl->addWidget(phone_title);

    phone_input_ = new QLineEdit;
    phone_input_->setPlaceholderText("+1 234 567 8900");
    phone_input_->setMinimumHeight(44);
    phone_input_->setStyleSheet(
        "QLineEdit { border: 1px solid #e5e7eb; border-radius: 8px; padding: 10px 14px; font-size: 15px; font-family: monospace; }"
        "QLineEdit:focus { border-color: #2563eb; }");
    pl->addWidget(phone_input_);

    phone_error_ = new QLabel;
    phone_error_->setStyleSheet("color: #dc2626; font-size: 12px;");
    phone_error_->hide();
    pl->addWidget(phone_error_);

    auto *phone_submit = new QPushButton("Continue");
    phone_submit->setMinimumHeight(44);
    phone_submit->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; }"
        "QPushButton:hover { background: #333; }");
    connect(phone_submit, &QPushButton::clicked, this, &AuthWidget::onPhoneSubmit);
    connect(phone_input_, &QLineEdit::returnPressed, this, &AuthWidget::onPhoneSubmit);
    pl->addWidget(phone_submit);

    auto *phone_back = new QPushButton("Back");
    phone_back->setFlat(true);
    phone_back->setStyleSheet("color: #6b7280; font-size: 13px;");
    connect(phone_back, &QPushButton::clicked, this, &AuthWidget::showWelcome);
    pl->addWidget(phone_back);

    pages_->addWidget(phone_page_);

    // --- Code page ---
    code_page_ = new QWidget;
    auto *cl = new QVBoxLayout(code_page_);
    cl->setAlignment(Qt::AlignCenter);
    cl->setSpacing(12);

    auto *code_title = new QLabel("Enter the code");
    code_title->setStyleSheet("font-size: 18px; font-weight: 600;");
    cl->addWidget(code_title);

    auto *code_hint = new QLabel("Sent to your Telegram app");
    code_hint->setStyleSheet("color: #6b7280; font-size: 13px;");
    cl->addWidget(code_hint);

    code_input_ = new QLineEdit;
    code_input_->setPlaceholderText("12345");
    code_input_->setMaxLength(6);
    code_input_->setMinimumHeight(44);
    code_input_->setStyleSheet(
        "QLineEdit { border: 1px solid #e5e7eb; border-radius: 8px; padding: 10px 14px; font-size: 20px; font-family: monospace; letter-spacing: 4px; text-align: center; }"
        "QLineEdit:focus { border-color: #2563eb; }");
    cl->addWidget(code_input_);

    code_error_ = new QLabel;
    code_error_->setStyleSheet("color: #dc2626; font-size: 12px;");
    code_error_->hide();
    cl->addWidget(code_error_);

    auto *code_submit = new QPushButton("Verify");
    code_submit->setMinimumHeight(44);
    code_submit->setStyleSheet(
        "QPushButton { background: #1a1a1a; color: white; border: none; border-radius: 8px; font-size: 14px; font-weight: 500; }"
        "QPushButton:hover { background: #333; }");
    connect(code_submit, &QPushButton::clicked, this, &AuthWidget::onCodeSubmit);
    connect(code_input_, &QLineEdit::returnPressed, this, &AuthWidget::onCodeSubmit);
    cl->addWidget(code_submit);

    auto *code_back = new QPushButton("Back");
    code_back->setFlat(true);
    code_back->setStyleSheet("color: #6b7280; font-size: 13px;");
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
    qr_image_->setFixedSize(200, 200);
    qr_image_->setAlignment(Qt::AlignCenter);
    qr_image_->setStyleSheet("background: #f3f4f6; border-radius: 10px;");
    ql->addWidget(qr_image_, 0, Qt::AlignCenter);

    auto *qr_hint = new QLabel("Open Telegram > Settings >\nDevices > Link Desktop Device");
    qr_hint->setAlignment(Qt::AlignCenter);
    qr_hint->setStyleSheet("color: #6b7280; font-size: 13px;");
    ql->addWidget(qr_hint);

    auto *qr_back = new QPushButton("Back");
    qr_back->setFlat(true);
    qr_back->setStyleSheet("color: #6b7280; font-size: 13px;");
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
    pages_->setCurrentWidget(welcome_page_);
}

void AuthWidget::showPhoneInput() {
    phone_error_->hide();
    phone_input_->clear();
    pages_->setCurrentWidget(phone_page_);
    phone_input_->setFocus();
}

void AuthWidget::showCodeInput() {
    code_error_->hide();
    code_input_->clear();
    pages_->setCurrentWidget(code_page_);
    code_input_->setFocus();
}

void AuthWidget::showQR() {
    pages_->setCurrentWidget(qr_page_);
    qr_timer_->start(1000);
}

void AuthWidget::showError(const std::string &msg) {
    auto current = pages_->currentWidget();
    if (current == phone_page_) {
        phone_error_->setText(QString::fromStdString(msg));
        phone_error_->show();
    } else if (current == code_page_) {
        code_error_->setText(QString::fromStdString(msg));
        code_error_->show();
    }
}

void AuthWidget::onPhoneSubmit() {
    QString phone = phone_input_->text().trimmed();
    phone.remove(QRegularExpression("[^+\\d]"));
    if (phone.isEmpty()) return;
    engine_->submit_phone(phone.toStdString());
}

void AuthWidget::onCodeSubmit() {
    QString code = code_input_->text().trimmed();
    if (code.isEmpty()) return;
    engine_->submit_code(code.toStdString());
}

void AuthWidget::onQRLogin() {
    engine_->request_qr_login();
    showQR();
}

void AuthWidget::pollQR() {
    std::string link = engine_->qr_link();
    if (link.empty()) return;

    // Simple QR rendering using a grid pattern
    // In production, use a proper QR library; here we show a placeholder with the link
    QPixmap pix(200, 200);
    pix.fill(QColor("#f3f4f6"));
    QPainter painter(&pix);
    painter.setPen(QColor("#6b7280"));
    auto font = painter.font();
    font.setPointSize(9);
    painter.setFont(font);
    painter.drawText(pix.rect(), Qt::AlignCenter | Qt::TextWordWrap,
        "Scan QR in\nTelegram app\n\n(Waiting...)");
    painter.end();
    qr_image_->setPixmap(pix);
}

} // namespace drive
