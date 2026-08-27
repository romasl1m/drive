#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QListWidget>
#include <QTreeWidget>
#include <QFileDialog>
#include <QProgressBar>

#include "../core/backup_engine.h"

namespace drive {

class AuthWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(BackupEngine *engine, QWidget *parent = nullptr);

    void onAuthStateChanged(BackupEngine::AuthState state);
    void onAuthError(const std::string &msg);
    void onProgressUpdate();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void refreshUI();
    void addFolder();
    void removeSelectedFolder();
    void restoreFolders();
    void trayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void setupTrayIcon();
    void setupDashboard();
    void showDashboard();
    void showAuth();
    void updateStats();
    void updateFolderList();
    void updateFileTree();

    BackupEngine *engine_;
    QStackedWidget *stack_;
    AuthWidget *auth_widget_;
    QWidget *dashboard_;

    // Dashboard widgets
    QLabel *status_label_;
    QLabel *stat_files_;
    QLabel *stat_storage_;
    QLabel *stat_folders_;
    QLabel *stat_largest_;
    QLabel *queue_label_;
    QListWidget *folder_list_;
    QTreeWidget *file_tree_;
    QPushButton *remove_btn_;
    QPushButton *restore_btn_;

    // Tray
    QSystemTrayIcon *tray_icon_;
    QMenu *tray_menu_;

    // Refresh timer
    QTimer *refresh_timer_;
};

} // namespace drive
