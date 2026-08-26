#include <QApplication>
#include <QLocalSocket>
#include <QLocalServer>
#include <thread>
#include <filesystem>
#include <cstring>

#include "config.h"
#include "core/backup_engine.h"
#include "gui/main_window.h"

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Drive");
    app.setApplicationVersion(drive::APP_VERSION);
    app.setOrganizationName("Drive");
    app.setQuitOnLastWindowClosed(false);

    bool start_hidden = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--background") == 0 || std::strcmp(argv[i], "--minimized") == 0)
            start_hidden = true;
    }

    // Single instance: if already running, tell it to show window
    QLocalSocket socket;
    socket.connectToServer("drive-desktop-instance");
    if (socket.waitForConnected(500)) {
        socket.write("show");
        socket.flush();
        socket.waitForBytesWritten();
        return 0;
    }

    QLocalServer server;
    QLocalServer::removeServer("drive-desktop-instance");
    server.listen("drive-desktop-instance");

    // Ensure data directories exist
    fs::create_directories(drive::data_dir());
    fs::create_directories(drive::config_dir());

    // Start backup engine on background thread
    drive::BackupEngine engine;
    std::thread engine_thread([&engine]() { engine.run(); });

    // Create main window
    drive::MainWindow window(&engine);

    // Wire callbacks from engine thread to Qt main thread
    engine.set_auth_callback([&window](drive::BackupEngine::AuthState state) {
        QMetaObject::invokeMethod(&window, [&window, state]() {
            window.onAuthStateChanged(state);
        }, Qt::QueuedConnection);
    });

    engine.set_error_callback([&window](const std::string &msg) {
        QMetaObject::invokeMethod(&window, [&window, msg]() {
            (void)window;
            (void)msg;
        }, Qt::QueuedConnection);
    });

    engine.set_progress_callback([&window]() {
        QMetaObject::invokeMethod(&window, [&window]() {
            window.onProgressUpdate();
        }, Qt::QueuedConnection);
    });

    // Handle second-instance showing window
    QObject::connect(&server, &QLocalServer::newConnection, [&window, &server]() {
        auto *conn = server.nextPendingConnection();
        if (conn) {
            window.show();
            window.raise();
            window.activateWindow();
            conn->deleteLater();
        }
    });

    if (!start_hidden)
        window.show();

    int ret = app.exec();

    engine.stop();
    engine_thread.join();
    return ret;
}
