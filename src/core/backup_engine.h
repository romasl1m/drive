#pragma once

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <functional>

#include <sys/inotify.h>
#include <sqlite3.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

namespace drive {

class BackupEngine {
public:
    enum class AuthState { NONE, WAIT_PHONE, WAIT_CODE, WAIT_QR, READY, CLOSED };

    struct FileRecord {
        std::int64_t message_id;
        std::int64_t file_size;
        std::string file_path;
        std::int64_t modified_time;
    };

    struct Stats {
        std::int64_t total_files = 0;
        std::int64_t total_size = 0;
        std::int64_t largest_file_size = 0;
        std::string largest_file_path;
        std::unordered_map<std::string, std::int64_t> type_count;
        std::unordered_map<std::string, std::int64_t> type_size;
        std::unordered_map<std::string, std::int64_t> folder_size;
        std::unordered_map<std::string, std::int64_t> folder_count;
    };

    using AuthCallback = std::function<void(AuthState)>;
    using ErrorCallback = std::function<void(const std::string &)>;
    using ProgressCallback = std::function<void()>;

    BackupEngine();
    ~BackupEngine();

    void run();
    void stop();

    // Auth
    AuthState auth_state() const { return auth_state_.load(); }
    std::string qr_link();
    std::string auth_error();
    void submit_phone(const std::string &phone);
    void submit_code(const std::string &code);
    void request_qr_login();
    void logout();

    // Callbacks
    void set_auth_callback(AuthCallback cb) { auth_cb_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_cb_ = std::move(cb); }
    void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

    // Folders
    std::vector<std::string> get_watched_folders();
    bool add_watched_folder(const std::string &path);
    bool remove_watched_folder(const std::string &path);

    // Files
    std::vector<FileRecord> get_backed_up_files();
    std::vector<FileRecord> get_files_under(const std::string &prefix);
    Stats get_stats();

    // Restore
    int restore_folder(const std::string &source, const std::string &dest);
    int restore_all(const std::string &dest);

    // Status
    std::size_t queue_size() const { return sync_queue_.size(); }
    bool ready() const { return is_ready_.load(); }

private:
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_ = 0;
    std::atomic<bool> is_running_{true};
    std::atomic<bool> is_ready_{false};
    bool is_busy_ = false;
    std::int64_t chat_id_ = 0;
    std::int64_t user_id_ = 0;

    std::atomic<AuthState> auth_state_{AuthState::NONE};
    std::mutex auth_mtx_;
    std::string qr_link_;
    std::string auth_error_;

    AuthCallback auth_cb_;
    ErrorCallback error_cb_;
    ProgressCallback progress_cb_;

    sqlite3 *db_ = nullptr;
    std::mutex db_mtx_;

    int inotify_fd_ = -1;
    std::unordered_map<int, std::string> wd_to_path_;

    std::vector<std::int64_t> search_chat_ids_;
    std::size_t search_index_ = 0;

    enum class SyncAction { UPLOAD, UPDATE, DELETE };
    struct SyncTask {
        SyncAction action;
        std::string file_path;
        std::int64_t old_message_id = 0;
    };
    std::queue<SyncTask> sync_queue_;
    SyncTask current_task_;

    std::mutex dl_mtx_;
    std::condition_variable dl_cv_;
    bool dl_completed_ = false;
    std::string dl_path_;
    std::int32_t dl_file_id_ = 0;

    std::unordered_map<std::int64_t, std::int32_t> msg_to_file_id_;
    bool history_loaded_ = false;

    // Init
    void init_database();
    void init_inotify();

    // Inotify
    void add_watch_recursive(const std::string &dir);
    void remove_watches_under(const std::string &prefix);
    void poll_inotify();
    void handle_inotify_event(struct inotify_event *ev);

    // Sync
    void schedule_file_sync(const std::string &path);
    void schedule_file_delete(const std::string &path);
    void process_queue();
    void perform_initial_scan();
    void scan_directory(const std::string &dir);
    void send_file(const std::string &fp);
    void delete_message(std::int64_t mid);

    // DB
    std::vector<std::string> db_get_watched_folders();
    void db_save_file(std::int64_t mid, std::int64_t sz, const std::string &fp, std::int64_t mt);
    void db_remove_file_by_path(const std::string &p);
    std::int64_t db_get_file_message(const std::string &p);
    bool db_file_unchanged(const std::string &p, std::int64_t sz, std::int64_t mt);
    std::vector<std::pair<std::string, std::int64_t>> db_get_all_files();
    std::int64_t db_get_channel(std::int64_t uid);
    void db_save_channel(std::int64_t uid, std::int64_t cid);

    // TDLib
    void process_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj);
    void handle_auth_state(td::td_api::object_ptr<td::td_api::AuthorizationState> state);
    void handle_chat_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj);
    void resolve_backup_channel();
    void search_backup_channel();
    void check_next_candidate();
    void start_watching();

    // Download
    std::string request_download(std::int64_t message_id);
    std::int32_t get_file_id_for_message(std::int64_t message_id);
    void load_chat_history_sync();
};

} // namespace drive
