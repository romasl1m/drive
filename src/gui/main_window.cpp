#include "main_window.h"
#include "auth_widget.h"
#include "../config.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHeaderView>
#include <QMessageBox>
#include <QStyle>
#include <QGroupBox>
#include <QSplitter>
#include <QFrame>

namespace drive {

static QString formatSize(int64_t bytes) {
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1048576) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1073741824) return QString::number(bytes / 1048576.0, 'f', 1) + " MB";
    return QString::number(bytes / 1073741824.0, 'f', 2) + " GB";
}

MainWindow::MainWindow(BackupEngine *engine, QWidget *parent)
    : QMainWindow(parent), engine_(engine) {
    setWindowTitle("Drive");
    setMinimumSize(700, 520);
    resize(800, 580);

    // Style the whole app
    setStyleSheet(R"(
        QMainWindow { background: #fafafa; }
        QLabel { color: #1a1a1a; }
        QGroupBox { font-weight: 600; font-size: 13px; border: 1px solid #e5e7eb; border-radius: 10px; margin-top: 12px; padding-top: 16px; background: white; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; color: #6b7280; }
        QListWidget { border: 1px solid #e5e7eb; border-radius: 8px; background: white; font-size: 13px; }
        QListWidget::item { padding: 8px 12px; border-bottom: 1px solid #f3f4f6; }
        QListWidget::item:selected { background: #eff6ff; color: #1a1a1a; }
        QTreeWidget { border: 1px solid #e5e7eb; border-radius: 8px; background: white; font-size: 13px; }
        QTreeWidget::item { padding: 4px; }
        QPushButton { border-radius: 7px; padding: 8px 16px; font-size: 13px; font-weight: 500; }
    )");

    // Stack: auth / dashboard
    stack_ = new QStackedWidget;
    setCentralWidget(stack_);

    // Auth widget
    auth_widget_ = new AuthWidget(engine_, this);
    stack_->addWidget(auth_widget_);

    // Dashboard
    setupDashboard();
    stack_->addWidget(dashboard_);

    // Tray icon
    setupTrayIcon();

    // Refresh timer
    refresh_timer_ = new QTimer(this);
    connect(refresh_timer_, &QTimer::timeout, this, &MainWindow::refreshUI);
    refresh_timer_->start(3000);

    // Check initial auth state
    auto state = engine_->auth_state();
    if (state == BackupEngine::AuthState::READY) {
        showDashboard();
    } else {
        showAuth();
    }
}

void MainWindow::setupDashboard() {
    dashboard_ = new QWidget;
    auto *main_layout = new QVBoxLayout(dashboard_);
    main_layout->setContentsMargins(24, 20, 24, 20);
    main_layout->setSpacing(16);

    // --- Header ---
    auto *header = new QHBoxLayout;
    auto *header_left = new QVBoxLayout;
    auto *app_title = new QLabel("Drive");
    auto tf = app_title->font();
    tf.setPointSize(18);
    tf.setBold(true);
    app_title->setFont(tf);
    header_left->addWidget(app_title);

    status_label_ = new QLabel;
    status_label_->setStyleSheet("font-size: 12px; color: #6b7280;");
    header_left->addWidget(status_label_);
    header->addLayout(header_left);

    header->addStretch();

    queue_label_ = new QLabel;
    queue_label_->setStyleSheet("font-size: 11px; color: #ea580c; background: #fff7ed; padding: 3px 8px; border-radius: 8px;");
    queue_label_->hide();
    header->addWidget(queue_label_);

    auto *logout_btn = new QPushButton("Sign out");
    logout_btn->setStyleSheet("QPushButton { color: #dc2626; background: transparent; border: 1px solid #fecaca; } QPushButton:hover { background: #fef2f2; }");
    connect(logout_btn, &QPushButton::clicked, this, [this]() {
        engine_->logout();
        showAuth();
    });
    header->addWidget(logout_btn);
    main_layout->addLayout(header);

    // --- Stats row ---
    auto *stats_layout = new QHBoxLayout;
    stats_layout->setSpacing(12);

    auto make_stat = [](const QString &label, QLabel *&value_label) -> QWidget* {
        auto *card = new QFrame;
        card->setStyleSheet("QFrame { background: white; border: 1px solid #e5e7eb; border-radius: 10px; padding: 14px; }");
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 12, 14, 12);
        cl->setSpacing(4);
        auto *lbl = new QLabel(label);
        lbl->setStyleSheet("font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: #6b7280;");
        cl->addWidget(lbl);
        value_label = new QLabel("-");
        value_label->setStyleSheet("font-size: 20px; font-weight: 700;");
        cl->addWidget(value_label);
        return card;
    };

    stats_layout->addWidget(make_stat("Files", stat_files_));
    stats_layout->addWidget(make_stat("Storage", stat_storage_));
    stats_layout->addWidget(make_stat("Folders", stat_folders_));
    stats_layout->addWidget(make_stat("Largest", stat_largest_));
    main_layout->addLayout(stats_layout);

    // --- Content: Folders + Files ---
    auto *content = new QHBoxLayout;
    content->setSpacing(16);

    // Left: Folders
    auto *folder_box = new QGroupBox("Watched Folders");
    auto *fl = new QVBoxLayout(folder_box);

    folder_list_ = new QListWidget;
    fl->addWidget(folder_list_);

    auto *folder_btns = new QHBoxLayout;
    auto *add_btn = new QPushButton("Add folder...");
    add_btn->setStyleSheet("QPushButton { background: #1a1a1a; color: white; border: none; } QPushButton:hover { background: #333; }");
    connect(add_btn, &QPushButton::clicked, this, &MainWindow::addFolder);
    folder_btns->addWidget(add_btn);

    remove_btn_ = new QPushButton("Remove");
    remove_btn_->setStyleSheet("QPushButton { color: #dc2626; background: transparent; border: 1px solid #fecaca; } QPushButton:hover { background: #fef2f2; }");
    remove_btn_->setEnabled(false);
    connect(remove_btn_, &QPushButton::clicked, this, &MainWindow::removeSelectedFolder);
    folder_btns->addWidget(remove_btn_);
    fl->addLayout(folder_btns);

    connect(folder_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        remove_btn_->setEnabled(row >= 0);
    });

    content->addWidget(folder_box, 1);

    // Right: File tree
    auto *file_box = new QGroupBox("Backed Up Files");
    auto *ftl = new QVBoxLayout(file_box);

    file_tree_ = new QTreeWidget;
    file_tree_->setHeaderLabels({"Name", "Size"});
    file_tree_->header()->setStretchLastSection(false);
    file_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    file_tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    file_tree_->setRootIsDecorated(true);
    ftl->addWidget(file_tree_);

    content->addWidget(file_box, 2);
    main_layout->addLayout(content, 1);
}

void MainWindow::setupTrayIcon() {
    tray_icon_ = new QSystemTrayIcon(this);
    tray_icon_->setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon));
    tray_icon_->setToolTip("Drive - Backup active");

    tray_menu_ = new QMenu(this);
    tray_menu_->addAction("Open Drive", this, [this]() { show(); raise(); activateWindow(); });
    tray_menu_->addSeparator();
    tray_menu_->addAction("Quit", this, [this]() {
        engine_->stop();
        QApplication::quit();
    });
    tray_icon_->setContextMenu(tray_menu_);

    connect(tray_icon_, &QSystemTrayIcon::activated, this, &MainWindow::trayActivated);
    tray_icon_->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    hide();
    tray_icon_->showMessage("Drive", "Backup continues in the background.", QSystemTrayIcon::Information, 2000);
    event->ignore();
}

void MainWindow::trayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        show();
        raise();
        activateWindow();
    }
}

void MainWindow::onAuthStateChanged(BackupEngine::AuthState state) {
    switch (state) {
    case BackupEngine::AuthState::WAIT_PHONE:
        auth_widget_->showPhoneInput();
        break;
    case BackupEngine::AuthState::WAIT_CODE:
        auth_widget_->showCodeInput();
        break;
    case BackupEngine::AuthState::WAIT_QR:
        auth_widget_->showQR();
        break;
    case BackupEngine::AuthState::READY:
        showDashboard();
        break;
    case BackupEngine::AuthState::CLOSED:
        showAuth();
        break;
    default:
        break;
    }
}

void MainWindow::onProgressUpdate() {
    refreshUI();
}

void MainWindow::refreshUI() {
    if (stack_->currentWidget() != dashboard_) return;
    updateStats();
    updateFolderList();
    updateFileTree();

    // Status
    bool ready = engine_->ready();
    if (ready) {
        status_label_->setText("Synced");
        status_label_->setStyleSheet("font-size: 12px; color: #16a34a; font-weight: 500;");
        tray_icon_->setToolTip("Drive - Backup active");
    } else {
        status_label_->setText("Connecting...");
        status_label_->setStyleSheet("font-size: 12px; color: #6b7280;");
        tray_icon_->setToolTip("Drive - Connecting");
    }

    auto q = engine_->queue_size();
    if (q > 0) {
        queue_label_->setText(QString::number(q) + " uploading");
        queue_label_->show();
    } else {
        queue_label_->hide();
    }
}

void MainWindow::showDashboard() {
    stack_->setCurrentWidget(dashboard_);
    refreshUI();
}

void MainWindow::showAuth() {
    auth_widget_->showWelcome();
    stack_->setCurrentWidget(auth_widget_);
}

void MainWindow::addFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select folder to back up",
        QDir::homePath(), QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;
    if (!engine_->add_watched_folder(dir.toStdString())) {
        QMessageBox::warning(this, "Error", "Could not add folder. Make sure it exists and is accessible.");
        return;
    }
    refreshUI();
}

void MainWindow::removeSelectedFolder() {
    auto *item = folder_list_->currentItem();
    if (!item) return;
    engine_->remove_watched_folder(item->text().toStdString());
    refreshUI();
}

void MainWindow::updateStats() {
    auto stats = engine_->get_stats();
    stat_files_->setText(QString::number(stats.total_files));
    stat_storage_->setText(formatSize(stats.total_size));
    stat_folders_->setText(QString::number(stats.folder_count.size()));
    if (stats.largest_file_size > 0) {
        QString name = QString::fromStdString(stats.largest_file_path);
        int idx = name.lastIndexOf('/');
        if (idx >= 0) name = name.mid(idx + 1);
        stat_largest_->setText(formatSize(stats.largest_file_size));
        stat_largest_->setToolTip(name);
    } else {
        stat_largest_->setText("-");
    }
}

void MainWindow::updateFolderList() {
    auto folders = engine_->get_watched_folders();
    auto stats = engine_->get_stats();

    int current_row = folder_list_->currentRow();
    folder_list_->clear();
    for (auto &f : folders) {
        QString label = QString::fromStdString(f);
        auto it = stats.folder_count.find(f);
        if (it != stats.folder_count.end()) {
            auto sit = stats.folder_size.find(f);
            int64_t sz = (sit != stats.folder_size.end()) ? sit->second : 0;
            label += "  (" + QString::number(it->second) + " files, " + formatSize(sz) + ")";
        }
        folder_list_->addItem(label);
    }
    if (current_row >= 0 && current_row < folder_list_->count())
        folder_list_->setCurrentRow(current_row);
}

void MainWindow::updateFileTree() {
    auto files = engine_->get_backed_up_files();
    auto folders = engine_->get_watched_folders();

    file_tree_->clear();
    if (files.empty()) return;

    // Build tree structure
    std::unordered_map<std::string, QTreeWidgetItem*> dir_items;

    for (auto &folder : folders) {
        QString folder_name = QString::fromStdString(folder).split('/').last();
        auto *root = new QTreeWidgetItem(file_tree_, {folder_name, ""});
        root->setExpanded(true);
        dir_items[folder] = root;
    }

    for (auto &f : files) {
        // Find parent folder
        QTreeWidgetItem *parent = nullptr;
        std::string parent_folder;
        for (auto &folder : folders) {
            if (f.file_path.rfind(folder, 0) == 0) {
                parent = dir_items[folder];
                parent_folder = folder;
                break;
            }
        }
        if (!parent) continue;

        std::string rel = f.file_path.substr(parent_folder.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);

        // Create intermediate directories
        auto parts = QString::fromStdString(rel).split('/');
        QTreeWidgetItem *current = parent;
        std::string built_path = parent_folder;
        for (int i = 0; i < parts.size() - 1; i++) {
            built_path += "/" + parts[i].toStdString();
            if (dir_items.find(built_path) == dir_items.end()) {
                auto *dir = new QTreeWidgetItem(current, {parts[i], ""});
                dir_items[built_path] = dir;
            }
            current = dir_items[built_path];
        }

        // Add file
        auto *item = new QTreeWidgetItem(current, {parts.last(), formatSize(f.file_size)});
        item->setForeground(1, QColor("#6b7280"));
    }
}

} // namespace drive
