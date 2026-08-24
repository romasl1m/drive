#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <sys/inotify.h>
#include <unistd.h>
#include <crow.h>
#include <sqlite3.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

namespace fs = std::filesystem;

class DriveBackup {
  public:
    enum class AuthState { NONE,
                           WAIT_PHONE,
                           WAIT_CODE,
                           WAIT_QR,
                           READY,
                           CLOSED };

    DriveBackup() {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        client_manager_ = std::make_unique<td::ClientManager>();
        client_id_ = client_manager_->create_client_id();

        init_database();
        init_inotify();

        auto get_version = td::td_api::make_object<td::td_api::getOption>();
        get_version->name_ = "version";
        client_manager_->send(client_id_, 0, std::move(get_version));
    }

    ~DriveBackup() {
        if (db_)
            sqlite3_close(db_);
        if (inotify_fd_ >= 0)
            close(inotify_fd_);
    }

    void run() {
        while (is_running_) {
            auto response = client_manager_->receive(0.1);
            if (response.object != nullptr) {
                process_response(response.request_id, std::move(response.object));
            }
            if (is_ready_ && !is_busy_) {
                poll_inotify();
                process_queue();
            }
        }
    }

    void stop() { is_running_ = false; }

    // --- auth API ---

    AuthState auth_state() const { return auth_state_.load(); }

    std::string qr_link() {
        std::lock_guard<std::mutex> lock(auth_mtx_);
        return qr_link_;
    }

    std::string auth_error() {
        std::lock_guard<std::mutex> lock(auth_mtx_);
        return auth_error_;
    }

    void submit_phone(const std::string &phone) {
        {
            std::lock_guard<std::mutex> lock(auth_mtx_);
            auth_error_.clear();
        }
        auto r = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
        r->phone_number_ = phone;
        r->settings_ = td::td_api::make_object<td::td_api::phoneNumberAuthenticationSettings>();
        client_manager_->send(client_id_, 2, std::move(r));
    }

    void submit_code(const std::string &code) {
        {
            std::lock_guard<std::mutex> lock(auth_mtx_);
            auth_error_.clear();
        }
        auto r = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
        r->code_ = code;
        client_manager_->send(client_id_, 3, std::move(r));
    }

    void request_qr_login() {
        {
            std::lock_guard<std::mutex> lock(auth_mtx_);
            auth_error_.clear();
        }
        auto r = td::td_api::make_object<td::td_api::requestQrCodeAuthentication>();
        client_manager_->send(client_id_, 2, std::move(r));
    }

    void logout() {
        client_manager_->send(client_id_, 300, td::td_api::make_object<td::td_api::logOut>());
    }

    // --- public API ---

    std::vector<std::string> get_watched_folders() {
        std::lock_guard<std::mutex> lock(db_mtx_);
        return db_get_watched_folders();
    }

    bool add_watched_folder(const std::string &path) {
        std::error_code ec;
        if (!fs::is_directory(path, ec))
            return false;
        {
            std::lock_guard<std::mutex> lock(db_mtx_);
            sqlite3_stmt *stmt;
            const char *sql = "INSERT OR IGNORE INTO watched_folders (folder_path) VALUES (?);";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return false;
            sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        add_watch_recursive(path);
        if (is_ready_)
            scan_directory(path);
        return true;
    }

    bool remove_watched_folder(const std::string &path) {
        std::lock_guard<std::mutex> lock(db_mtx_);
        sqlite3_stmt *stmt;
        const char *sql = "DELETE FROM watched_folders WHERE folder_path = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        remove_watches_under(path);
        return true;
    }

    struct FileRecord {
        std::int64_t message_id;
        std::int64_t file_size;
        std::string file_path;
        std::int64_t modified_time;
    };

    std::vector<FileRecord> get_backed_up_files() {
        std::lock_guard<std::mutex> lock(db_mtx_);
        std::vector<FileRecord> result;
        sqlite3_stmt *stmt;
        const char *sql = "SELECT message_id, file_size, file_path, modified_time FROM files ORDER BY file_path;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return result;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FileRecord r;
            r.message_id = sqlite3_column_int64(stmt, 0);
            r.file_size = sqlite3_column_int64(stmt, 1);
            const char *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            r.file_path = p ? p : "";
            r.modified_time = sqlite3_column_int64(stmt, 3);
            result.push_back(r);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    std::vector<FileRecord> get_files_under(const std::string &prefix) {
        std::lock_guard<std::mutex> lock(db_mtx_);
        std::vector<FileRecord> result;
        sqlite3_stmt *stmt;
        const char *sql = "SELECT message_id, file_size, file_path, modified_time FROM files WHERE file_path LIKE ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return result;
        std::string pattern = prefix + "%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FileRecord r;
            r.message_id = sqlite3_column_int64(stmt, 0);
            r.file_size = sqlite3_column_int64(stmt, 1);
            const char *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            r.file_path = p ? p : "";
            r.modified_time = sqlite3_column_int64(stmt, 3);
            result.push_back(r);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    std::size_t queue_size() { return sync_queue_.size(); }
    bool ready() const { return is_ready_; }

    int restore_folder(const std::string &source_folder, const std::string &dest) {
        auto files = get_files_under(source_folder);
        if (files.empty())
            return 0;
        std::error_code ec;
        fs::create_directories(dest, ec);
        int count = 0;
        for (auto &f : files) {
            std::string rel = f.file_path.substr(source_folder.size());
            if (!rel.empty() && rel[0] == '/')
                rel = rel.substr(1);
            std::string dest_path = dest + "/" + rel;
            fs::create_directories(fs::path(dest_path).parent_path(), ec);
            if (fs::exists(f.file_path, ec)) {
                fs::copy_file(f.file_path, dest_path, fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    count++;
            } else {
                std::string dl = request_download(f.message_id);
                if (!dl.empty()) {
                    fs::copy_file(dl, dest_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec)
                        count++;
                }
            }
        }
        return count;
    }

    int restore_all(const std::string &dest) {
        auto folders = get_watched_folders();
        int total = 0;
        for (auto &folder : folders) {
            total += restore_folder(folder, dest + "/" + fs::path(folder).filename().string());
        }
        return total;
    }

    std::string request_download(std::int64_t message_id) {
        std::int32_t file_id = get_file_id_for_message(message_id);
        if (file_id == 0)
            return "";
        {
            std::lock_guard<std::mutex> lock(dl_mtx_);
            dl_completed_ = false;
            dl_path_.clear();
            dl_file_id_ = file_id;
        }
        auto request = td::td_api::make_object<td::td_api::downloadFile>();
        request->file_id_ = file_id;
        request->priority_ = 32;
        request->offset_ = 0;
        request->limit_ = 0;
        request->synchronous_ = true;
        client_manager_->send(client_id_, 200, std::move(request));
        std::unique_lock<std::mutex> lock(dl_mtx_);
        dl_cv_.wait_for(lock, std::chrono::seconds(300), [this] { return dl_completed_; });
        return dl_path_;
    }

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

    sqlite3 *db_ = nullptr;
    std::mutex db_mtx_;

    int inotify_fd_ = -1;
    std::unordered_map<int, std::string> wd_to_path_;

    std::vector<std::int64_t> search_chat_ids_;
    std::size_t search_index_ = 0;

    enum class SyncAction { UPLOAD,
                            UPDATE,
                            DELETE };
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

    std::int32_t get_file_id_for_message(std::int64_t message_id) {
        if (!history_loaded_)
            load_chat_history_sync();
        auto it = msg_to_file_id_.find(message_id);
        if (it != msg_to_file_id_.end())
            return it->second;
        load_chat_history_sync();
        it = msg_to_file_id_.find(message_id);
        return (it != msg_to_file_id_.end()) ? it->second : 0;
    }

    void load_chat_history_sync() {
        auto request = td::td_api::make_object<td::td_api::getChatHistory>();
        request->chat_id_ = chat_id_;
        request->from_message_id_ = 0;
        request->offset_ = 0;
        request->limit_ = 100;
        request->only_local_ = false;
        client_manager_->send(client_id_, 201, std::move(request));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    void init_inotify() {
        inotify_fd_ = inotify_init1(IN_NONBLOCK);
        if (inotify_fd_ < 0) {
            std::cerr << "[ERROR] inotify_init" << std::endl;
            exit(1);
        }
    }

    void add_watch_recursive(const std::string &dir) {
        int wd = inotify_add_watch(inotify_fd_, dir.c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
        if (wd < 0)
            return;
        wd_to_path_[wd] = dir;
        std::error_code ec;
        for (auto &entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_directory(ec))
                add_watch_recursive(entry.path().string());
        }
    }

    void remove_watches_under(const std::string &prefix) {
        std::vector<int> rm;
        for (auto &[wd, path] : wd_to_path_) {
            if (path == prefix || path.rfind(prefix + "/", 0) == 0) {
                inotify_rm_watch(inotify_fd_, wd);
                rm.push_back(wd);
            }
        }
        for (int w : rm)
            wd_to_path_.erase(w);
    }

    std::vector<std::string> db_get_watched_folders() {
        std::vector<std::string> r;
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_, "SELECT folder_path FROM watched_folders ORDER BY folder_path;", -1, &stmt, nullptr) != SQLITE_OK)
            return r;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (p)
                r.emplace_back(p);
        }
        sqlite3_finalize(stmt);
        return r;
    }

    void poll_inotify() {
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        while (true) {
            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len <= 0)
                break;
            char *ptr = buf;
            while (ptr < buf + len) {
                auto *ev = reinterpret_cast<struct inotify_event *>(ptr);
                handle_inotify_event(ev);
                ptr += sizeof(struct inotify_event) + ev->len;
            }
        }
    }

    void handle_inotify_event(struct inotify_event *ev) {
        if (ev->mask & IN_ISDIR) {
            if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                std::string d = wd_to_path_[ev->wd] + "/" + ev->name;
                add_watch_recursive(d);
                scan_directory(d);
            }
            return;
        }
        if (!ev->len)
            return;
        std::string fp = wd_to_path_[ev->wd] + "/" + ev->name;
        if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
            schedule_file_sync(fp);
        else if (ev->mask & (IN_DELETE | IN_MOVED_FROM))
            schedule_file_delete(fp);
    }

    void schedule_file_sync(const std::string &path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return;
        std::int64_t size = static_cast<std::int64_t>(fs::file_size(path, ec));
        auto ft = fs::last_write_time(path, ec);
        std::int64_t mtime = std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
        std::int64_t mid = db_get_file_message(path);
        if (mid != 0) {
            if (db_file_unchanged(path, size, mtime))
                return;
            sync_queue_.push({SyncAction::UPDATE, path, mid});
        } else
            sync_queue_.push({SyncAction::UPLOAD, path, 0});
    }

    void schedule_file_delete(const std::string &path) {
        std::int64_t mid = db_get_file_message(path);
        if (mid == 0)
            return;
        sync_queue_.push({SyncAction::DELETE, path, mid});
    }

    void process_queue() {
        if (sync_queue_.empty() || is_busy_)
            return;
        current_task_ = sync_queue_.front();
        sync_queue_.pop();
        is_busy_ = true;
        switch (current_task_.action) {
        case SyncAction::UPLOAD:
            send_file(current_task_.file_path);
            break;
        case SyncAction::UPDATE:
        case SyncAction::DELETE:
            delete_message(current_task_.old_message_id);
            break;
        }
    }

    void perform_initial_scan() {
        std::vector<std::string> folders;
        {
            std::lock_guard<std::mutex> lock(db_mtx_);
            folders = db_get_watched_folders();
        }
        std::unordered_set<std::string> fs_files;
        std::error_code ec;
        for (auto &folder : folders) {
            add_watch_recursive(folder);
            for (auto &entry : fs::recursive_directory_iterator(folder, ec)) {
                if (entry.is_regular_file(ec)) {
                    fs_files.insert(entry.path().string());
                    schedule_file_sync(entry.path().string());
                }
            }
        }
        auto db_files = db_get_all_files();
        for (auto &[path, mid] : db_files) {
            if (fs_files.find(path) == fs_files.end()) {
                bool w = false;
                for (auto &f : folders) {
                    if (path.rfind(f, 0) == 0) {
                        w = true;
                        break;
                    }
                }
                if (w)
                    schedule_file_delete(path);
                else {
                    std::lock_guard<std::mutex> lock(db_mtx_);
                    db_remove_file_by_path(path);
                }
            }
        }
    }

    void scan_directory(const std::string &dir) {
        std::error_code ec;
        for (auto &e : fs::recursive_directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec))
                schedule_file_sync(e.path().string());
        }
    }

    void send_file(const std::string &fp) {
        auto req = td::td_api::make_object<td::td_api::sendMessage>();
        req->chat_id_ = chat_id_;
        req->input_message_content_ = td::td_api::make_object<td::td_api::inputMessageDocument>(
            td::td_api::make_object<td::td_api::inputDocument>(td::td_api::make_object<td::td_api::inputFileLocal>(fp), nullptr, false),
            td::td_api::make_object<td::td_api::formattedText>());
        client_manager_->send(client_id_, 100, std::move(req));
    }

    void delete_message(std::int64_t mid) {
        auto req = td::td_api::make_object<td::td_api::deleteMessages>();
        req->chat_id_ = chat_id_;
        req->message_ids_ = {mid};
        req->revoke_ = true;
        client_manager_->send(client_id_, 101, std::move(req));
    }

    void init_database() {
        sqlite3_open("backup.db", &db_);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;"
                          "CREATE TABLE IF NOT EXISTS channels(user_id INTEGER PRIMARY KEY,chat_id INTEGER NOT NULL);"
                          "CREATE TABLE IF NOT EXISTS files(message_id INTEGER PRIMARY KEY,file_size INTEGER NOT NULL,file_path TEXT NOT NULL UNIQUE,modified_time INTEGER NOT NULL);"
                          "CREATE TABLE IF NOT EXISTS watched_folders(folder_path TEXT PRIMARY KEY);",
                     nullptr, nullptr, nullptr);
    }

    std::int64_t db_get_channel(std::int64_t uid) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "SELECT chat_id FROM channels WHERE user_id=?", -1, &s, nullptr) != SQLITE_OK)
            return 0;
        sqlite3_bind_int64(s, 1, uid);
        std::int64_t r = 0;
        if (sqlite3_step(s) == SQLITE_ROW)
            r = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return r;
    }
    void db_save_channel(std::int64_t uid, std::int64_t cid) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO channels(user_id,chat_id)VALUES(?,?)", -1, &s, nullptr) != SQLITE_OK)
            return;
        sqlite3_bind_int64(s, 1, uid);
        sqlite3_bind_int64(s, 2, cid);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    void db_save_file(std::int64_t mid, std::int64_t sz, const std::string &fp, std::int64_t mt) {
        std::lock_guard<std::mutex> lock(db_mtx_);
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO files(message_id,file_size,file_path,modified_time)VALUES(?,?,?,?)", -1, &s, nullptr) != SQLITE_OK)
            return;
        sqlite3_bind_int64(s, 1, mid);
        sqlite3_bind_int64(s, 2, sz);
        sqlite3_bind_text(s, 3, fp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, mt);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    void db_remove_file_by_path(const std::string &p) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "DELETE FROM files WHERE file_path=?", -1, &s, nullptr) != SQLITE_OK)
            return;
        sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    std::int64_t db_get_file_message(const std::string &p) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "SELECT message_id FROM files WHERE file_path=?", -1, &s, nullptr) != SQLITE_OK)
            return 0;
        sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        std::int64_t r = 0;
        if (sqlite3_step(s) == SQLITE_ROW)
            r = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return r;
    }
    bool db_file_unchanged(const std::string &p, std::int64_t sz, std::int64_t mt) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM files WHERE file_path=? AND file_size=? AND modified_time=?", -1, &s, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, sz);
        sqlite3_bind_int64(s, 3, mt);
        bool r = (sqlite3_step(s) == SQLITE_ROW);
        sqlite3_finalize(s);
        return r;
    }
    std::vector<std::pair<std::string, std::int64_t>> db_get_all_files() {
        std::vector<std::pair<std::string, std::int64_t>> r;
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db_, "SELECT file_path,message_id FROM files", -1, &s, nullptr) != SQLITE_OK)
            return r;
        while (sqlite3_step(s) == SQLITE_ROW) {
            const char *p = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
            if (p)
                r.emplace_back(p, sqlite3_column_int64(s, 1));
        }
        sqlite3_finalize(s);
        return r;
    }

    // --- TDLib responses ---
    void process_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj) {
        if (!obj)
            return;
        std::int32_t id = obj->get_id();

        if (id == td::td_api::updateAuthorizationState::ID) {
            auto st = td::move_tl_object_as<td::td_api::updateAuthorizationState>(obj);
            handle_auth_state(std::move(st->authorization_state_));
        } else if (id == td::td_api::user::ID && rid == 12) {
            auto u = td::move_tl_object_as<td::td_api::user>(obj);
            user_id_ = u->id_;
            resolve_backup_channel();
        } else if (id == td::td_api::chats::ID && rid == 10) {
            auto c = td::move_tl_object_as<td::td_api::chats>(obj);
            search_chat_ids_ = std::move(c->chat_ids_);
            search_index_ = 0;
            check_next_candidate();
        } else if (id == td::td_api::chat::ID) {
            handle_chat_response(rid, std::move(obj));
        } else if (id == td::td_api::message::ID && rid == 100) {
            auto m = td::move_tl_object_as<td::td_api::message>(obj);
            std::error_code ec;
            std::int64_t sz = 0, mt = 0;
            if (fs::exists(current_task_.file_path, ec)) {
                sz = static_cast<std::int64_t>(fs::file_size(current_task_.file_path, ec));
                mt = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(current_task_.file_path, ec).time_since_epoch()).count();
            }
            db_save_file(m->id_, sz, current_task_.file_path, mt);
            is_busy_ = false;
        } else if (id == td::td_api::ok::ID && rid == 101) {
            {
                std::lock_guard<std::mutex> lock(db_mtx_);
                db_remove_file_by_path(current_task_.file_path);
            }
            if (current_task_.action == SyncAction::UPDATE)
                send_file(current_task_.file_path);
            else
                is_busy_ = false;
        } else if (id == td::td_api::file::ID && rid == 200) {
            auto f = td::move_tl_object_as<td::td_api::file>(obj);
            std::lock_guard<std::mutex> lock(dl_mtx_);
            if (f->local_ && f->local_->is_downloading_completed_)
                dl_path_ = f->local_->path_;
            dl_completed_ = true;
            dl_cv_.notify_all();
        } else if (id == td::td_api::messages::ID && rid == 201) {
            auto ms = td::move_tl_object_as<td::td_api::messages>(obj);
            for (auto &m : ms->messages_) {
                if (!m || !m->content_)
                    continue;
                auto cid = m->content_->get_id();
                if (cid == td::td_api::messageDocument::ID) {
                    auto *d = static_cast<td::td_api::messageDocument *>(m->content_.get());
                    if (d->document_)
                        msg_to_file_id_[m->id_] = d->document_->document_->id_;
                } else if (cid == td::td_api::messagePhoto::ID) {
                    auto *p = static_cast<td::td_api::messagePhoto *>(m->content_.get());
                    if (p->photo_ && !p->photo_->sizes_.empty())
                        msg_to_file_id_[m->id_] = p->photo_->sizes_.back()->photo_->id_;
                } else if (cid == td::td_api::messageVideo::ID) {
                    auto *v = static_cast<td::td_api::messageVideo *>(m->content_.get());
                    if (v->video_)
                        msg_to_file_id_[m->id_] = v->video_->video_->id_;
                }
            }
            history_loaded_ = true;
        } else if (id == td::td_api::error::ID) {
            auto e = td::move_tl_object_as<td::td_api::error>(obj);
            std::cerr << "[ERROR] " << rid << ": " << e->message_ << std::endl;
            if (rid == 2 || rid == 3) {
                std::lock_guard<std::mutex> lock(auth_mtx_);
                auth_error_ = e->message_;
            } else if (rid == 200) {
                std::lock_guard<std::mutex> lock(dl_mtx_);
                dl_completed_ = true;
                dl_cv_.notify_all();
            } else if (rid == 13)
                search_backup_channel();
            else if (rid == 100 || rid == 101)
                is_busy_ = false;
        }
    }

    void handle_chat_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj) {
        auto c = td::move_tl_object_as<td::td_api::chat>(obj);
        if (rid == 13) {
            chat_id_ = c->id_;
            start_watching();
        } else if (rid == 11) {
            if (c->title_ == "backup" && c->type_->get_id() == td::td_api::chatTypeSupergroup::ID) {
                auto *sg = static_cast<td::td_api::chatTypeSupergroup *>(c->type_.get());
                if (sg->is_channel_) {
                    chat_id_ = c->id_;
                    db_save_channel(user_id_, chat_id_);
                    start_watching();
                    return;
                }
            }
            search_index_++;
            check_next_candidate();
        } else if (rid == 4) {
            chat_id_ = c->id_;
            db_save_channel(user_id_, chat_id_);
            start_watching();
        }
    }

    void handle_auth_state(td::td_api::object_ptr<td::td_api::AuthorizationState> state) {
        if (!state)
            return;
        switch (state->get_id()) {
        case td::td_api::authorizationStateWaitTdlibParameters::ID: {
            const char *api_id = std::getenv("API_ID"), *api_hash = std::getenv("API_HASH");
            if (!api_id || !api_hash) {
                std::cerr << "[ERROR] Missing API_ID/API_HASH!" << std::endl;
                exit(1);
            }
            auto r = td::td_api::make_object<td::td_api::setTdlibParameters>();
            r->database_directory_ = "tdlib";
            r->use_message_database_ = true;
            r->use_secret_chats_ = true;
            r->api_id_ = std::stoi(api_id);
            r->api_hash_ = api_hash;
            r->system_language_code_ = "en";
            r->device_model_ = "drive";
            r->application_version_ = "1.0";
            client_manager_->send(client_id_, 1, std::move(r));
            break;
        }
        case td::td_api::authorizationStateWaitPhoneNumber::ID:
            auth_state_ = AuthState::WAIT_PHONE;
            break;
        case td::td_api::authorizationStateWaitCode::ID:
            auth_state_ = AuthState::WAIT_CODE;
            break;
        case td::td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
            auto s = td::move_tl_object_as<td::td_api::authorizationStateWaitOtherDeviceConfirmation>(state);
            {
                std::lock_guard<std::mutex> lock(auth_mtx_);
                qr_link_ = s->link_;
            }
            auth_state_ = AuthState::WAIT_QR;
            break;
        }
        case td::td_api::authorizationStateReady::ID:
            auth_state_ = AuthState::READY;
            client_manager_->send(client_id_, 12, td::td_api::make_object<td::td_api::getMe>());
            break;
        case td::td_api::authorizationStateClosed::ID:
            auth_state_ = AuthState::CLOSED;
            is_ready_ = false;
            break;
        default:
            break;
        }
    }

    void resolve_backup_channel() {
        auto id = db_get_channel(user_id_);
        if (id) {
            auto r = td::td_api::make_object<td::td_api::getChat>();
            r->chat_id_ = id;
            client_manager_->send(client_id_, 13, std::move(r));
        } else
            search_backup_channel();
    }
    void search_backup_channel() {
        auto r = td::td_api::make_object<td::td_api::searchChatsOnServer>();
        r->query_ = "backup";
        r->limit_ = 20;
        client_manager_->send(client_id_, 10, std::move(r));
    }
    void check_next_candidate() {
        if (search_index_ >= search_chat_ids_.size()) {
            auto r = td::td_api::make_object<td::td_api::createNewSupergroupChat>();
            r->title_ = "backup";
            r->is_channel_ = true;
            r->description_ = "drive backup";
            client_manager_->send(client_id_, 4, std::move(r));
            return;
        }
        auto r = td::td_api::make_object<td::td_api::getChat>();
        r->chat_id_ = search_chat_ids_[search_index_];
        client_manager_->send(client_id_, 11, std::move(r));
    }
    void start_watching() {
        is_ready_ = true;
        perform_initial_scan();
    }
};

// --- HTML ---

static const char *HTML_PAGE = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>drive</title>
<style>
:root { --bg: #fafafa; --fg: #1a1a1a; --muted: #6b7280; --faint: #c4c4c4; --border: #e5e7eb; --surface: #f3f4f6; }
@media (prefers-color-scheme: dark) {
    :root { --bg: #1a1a2e; --fg: #eaeaea; --muted: #9ca3af; --faint: #4b5563; --border: #2d2d44; --surface: #22223a; }
}
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'Inter', -apple-system, system-ui, sans-serif; background: var(--bg); color: var(--fg); min-height: 100vh; -webkit-font-smoothing: antialiased; font-size: 15px; }
.wrap { max-width: 800px; margin: 0 auto; padding: 56px 32px; }

/* Landing */
.landing { display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 90vh; text-align: center; gap: 12px; }
.landing h1 { font-size: 40px; font-weight: 700; letter-spacing: -0.5px; margin-bottom: 4px; }
.landing .sub { font-size: 16px; color: var(--muted); max-width: 440px; margin-bottom: 32px; line-height: 1.7; }
.landing .features { list-style: none; text-align: left; max-width: 380px; width: 100%; margin-bottom: 36px; display: flex; flex-direction: column; gap: 14px; }
.landing .features li { font-size: 15px; color: var(--muted); line-height: 1.5; padding-left: 20px; position: relative; }
.landing .features li::before { content: '\2022'; position: absolute; left: 0; color: var(--faint); font-size: 18px; }
.landing .methods { display: flex; flex-direction: column; gap: 16px; width: 100%; max-width: 360px; }
.landing .methods .or { font-size: 11px; color: var(--faint); text-transform: uppercase; letter-spacing: 1px; padding: 4px 0; }

/* Auth forms */
.auth-box { width: 100%; max-width: 360px; }
.auth-box h2 { font-size: 17px; font-weight: 500; margin-bottom: 20px; }
.auth-box input { width: 100%; border: 1px solid var(--border); background: var(--surface); color: var(--fg); padding: 14px 16px; border-radius: 8px; font-size: 15px; font-family: monospace; margin-bottom: 14px; }
.auth-box input:focus { outline: none; border-color: var(--fg); }
.auth-box .back { font-size: 13px; color: var(--muted); cursor: pointer; margin-top: 12px; display: inline-block; }
.auth-box .back:hover { color: var(--fg); }

.qr-box { display: flex; flex-direction: column; align-items: center; gap: 20px; }
.qr-box canvas { border-radius: 10px; }
.qr-box p { font-size: 13px; color: var(--muted); }

/* Buttons */
.btn { display: inline-flex; align-items: center; justify-content: center; border: 1px solid var(--border); background: var(--surface); color: var(--fg); padding: 12px 20px; border-radius: 8px; font-size: 14px; cursor: pointer; font-family: inherit; transition: all 0.15s; }
.btn:hover { border-color: var(--fg); background: var(--bg); }
.btn.solid { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.btn.solid:hover { opacity: 0.85; }
.btn.danger { color: #dc2626; border-color: #fecaca; }
.btn.danger:hover { border-color: #dc2626; }
@media (prefers-color-scheme: dark) { .btn.danger { color: #f87171; border-color: #7f1d1d; } .btn.danger:hover { border-color: #f87171; } }
.btn.sm { padding: 6px 12px; font-size: 13px; }

/* Dashboard */
.dash { display: none; }
header { margin-bottom: 48px; display: flex; align-items: center; justify-content: space-between; }
header .left h1 { font-size: 22px; font-weight: 600; letter-spacing: -0.3px; }
header .left p { font-size: 14px; color: var(--muted); margin-top: 4px; }
header .indicator { display: inline-flex; align-items: center; gap: 6px; font-size: 12px; color: var(--muted); margin-top: 8px; }
header .indicator .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--muted); }
header .indicator .dot.on { background: #22c55e; }
header .indicator .badge { background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 2px 8px; font-size: 11px; }
section { margin-bottom: 44px; }
section > label { display: block; font-size: 12px; font-weight: 500; text-transform: uppercase; letter-spacing: 0.5px; color: var(--muted); margin-bottom: 12px; }
.panel { border: 1px solid var(--border); border-radius: 10px; overflow: hidden; }
.row { display: flex; align-items: center; justify-content: space-between; padding: 14px 18px; border-bottom: 1px solid var(--border); font-size: 14px; }
.row:last-child { border-bottom: none; }
.row .path { font-family: monospace; font-size: 13px; }
.row .actions { display: flex; gap: 8px; }
.browser-bar { display: flex; align-items: center; padding: 12px 18px; border-bottom: 1px solid var(--border); gap: 10px; }
.browser-bar .crumb { font-family: monospace; font-size: 13px; color: var(--muted); }
.browser-list { max-height: 340px; overflow-y: auto; }
.browser-list .item { display: flex; align-items: center; gap: 10px; padding: 10px 18px; font-size: 14px; font-family: monospace; cursor: pointer; border-bottom: 1px solid var(--border); }
.browser-list .item:last-child { border-bottom: none; }
.browser-list .item:hover { background: var(--surface); }
.browser-list .item.file { color: var(--muted); cursor: default; }
.browser-list .item .icon { width: 14px; color: var(--faint); font-size: 12px; }
.tree-dir { padding: 10px 18px; cursor: pointer; display: flex; align-items: center; gap: 8px; border-bottom: 1px solid var(--border); user-select: none; font-family: monospace; font-size: 13px; }
.tree-dir:hover { background: var(--surface); }
.tree-dir .arrow { font-size: 9px; color: var(--muted); transition: transform 0.15s; width: 12px; }
.tree-dir.open .arrow { transform: rotate(90deg); }
.tree-dir .name { flex: 1; }
.tree-children { display: none; padding-left: 18px; }
.tree-dir.open + .tree-children { display: block; }
.tree-file { padding: 8px 18px; display: flex; align-items: center; gap: 10px; border-bottom: 1px solid var(--border); font-family: monospace; font-size: 13px; color: var(--muted); }
.tree-file:last-child { border-bottom: none; }
.tree-file .name { flex: 1; }
.tree-file .size { font-size: 12px; }
.restore-bar { display: flex; align-items: center; gap: 10px; padding: 14px 18px; border-bottom: 1px solid var(--border); }
.restore-bar input { flex: 1; border: 1px solid var(--border); background: var(--surface); color: var(--fg); padding: 10px 14px; border-radius: 7px; font-family: monospace; font-size: 13px; }
.restore-bar input:focus { outline: none; border-color: var(--fg); }
.status-msg { font-size: 13px; color: var(--muted); padding: 10px 18px; }
.empty { padding: 20px 18px; font-size: 14px; color: var(--muted); }
.section-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
.section-header label { margin-bottom: 0; }
.toggle { font-size: 12px; color: var(--muted); cursor: pointer; border: none; background: none; font-family: inherit; }
.toggle:hover { color: var(--fg); }
.hidden { display: none; }
</style>
</head>
<body>

<!-- Landing / Login -->
<div class="landing" id="loginView">
    <h1>drive</h1>
    <p class="sub">Private unlimited storage powered by Telegram. Your files are synced automatically and kept forever.</p>
    <ul class="features">
        <li>Unlimited storage with no file size limits</li>
        <li>Automatic sync — changes are detected and uploaded instantly</li>
        <li>Restore any file or folder at any time</li>
        <li>End-to-end private, stored in your own Telegram channel</li>
        <li>No subscriptions, no third-party servers</li>
    </ul>
    <div class="methods" id="loginMethods">
        <button class="btn solid" onclick="startPhone()">Sign in with phone number</button>
        <span class="or">or</span>
        <button class="btn" onclick="startQR()">Sign in with QR code</button>
    </div>
    <div class="auth-box hidden" id="phoneForm">
        <h2>Phone number</h2>
        <input type="tel" id="phoneInput" placeholder="+1 234 567 8900" autofocus>
        <button class="btn solid" style="width:100%" onclick="submitPhone()">Continue</button>
        <span class="back" onclick="showMethods()">Back</span>
    </div>
    <div class="auth-box hidden" id="codeForm">
        <h2>Enter the code</h2>
        <p style="font-size:12px;color:var(--muted);margin-bottom:12px">Sent to your Telegram app</p>
        <input type="text" id="codeInput" placeholder="12345" maxlength="6" autofocus>
        <button class="btn solid" style="width:100%" onclick="submitCode()">Verify</button>
        <span class="back" onclick="showMethods()">Back</span>
    </div>
    <div class="auth-box hidden" id="qrForm">
        <div class="qr-box">
            <h2>Scan with Telegram</h2>
            <canvas id="qrCanvas"></canvas>
            <p>Open Telegram &gt; Settings &gt; Devices &gt; Link Desktop Device</p>
        </div>
        <span class="back" onclick="showMethods()">Back</span>
    </div>
</div>

<!-- Dashboard -->
<div class="wrap dash" id="dashView">
    <header>
        <div class="left">
            <h1>drive</h1>
            <p>Private unlimited storage</p>
            <div class="indicator">
                <div class="dot" id="statusDot"></div>
                <span id="statusText">connecting</span>
                <span class="badge" id="queueBadge" style="display:none"></span>
            </div>
        </div>
        <button class="btn danger" onclick="doLogout()">Sign out</button>
    </header>

    <section>
        <label>Watched folders</label>
        <div class="panel" id="watchedPanel"><div class="empty">No folders added yet.</div></div>
    </section>

    <section>
        <label>Add folder</label>
        <div class="panel">
            <div class="browser-bar">
                <span class="crumb" id="currentPath">/home</span>
                <span style="flex:1"></span>
                <button class="btn solid sm" id="addFolderBtn">Add</button>
            </div>
            <div class="browser-list" id="browser"></div>
        </div>
    </section>

    <section>
        <div class="section-header">
            <label>Files</label>
            <button class="toggle" id="toggleFiles">hide</button>
        </div>
        <div id="filesSection">
            <div class="panel">
                <div class="restore-bar">
                    <input type="text" id="restorePath" placeholder="/path/to/restore" value="/home/roman/backup_test">
                    <button class="btn solid sm" onclick="restoreAll()">Restore all</button>
                </div>
                <div id="dlStatus" class="status-msg" style="display:none"></div>
                <div id="fileTree"></div>
            </div>
        </div>
    </section>
</div>

<script src="https://cdn.jsdelivr.net/npm/qrcode-generator@1.4.4/qrcode.min.js"></script>
<script>
let currentPath='/home';
let filesVisible=true;
let authPoll=null;

async function api(url,opts){return(await fetch(url,opts)).json();}

function showLogin(){document.getElementById('loginView').style.display='flex';document.getElementById('dashView').style.display='none';}
function showDash(){document.getElementById('loginView').style.display='none';document.getElementById('dashView').style.display='block';}

function showMethods(){
    document.getElementById('loginMethods').classList.remove('hidden');
    document.getElementById('phoneForm').classList.add('hidden');
    document.getElementById('codeForm').classList.add('hidden');
    document.getElementById('qrForm').classList.add('hidden');
}

function startPhone(){
    document.getElementById('loginMethods').classList.add('hidden');
    document.getElementById('phoneForm').classList.remove('hidden');
    document.getElementById('phoneInput').focus();
}

async function startQR(){
    document.getElementById('loginMethods').classList.add('hidden');
    document.getElementById('qrForm').classList.remove('hidden');
    await api('/api/auth/qr',{method:'POST'});
    pollAuth();
}

async function submitPhone(){
    const phone=document.getElementById('phoneInput').value.replace(/[^+\d]/g,'');
    if(!phone)return;
    await api('/api/auth/phone',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({phone})});
    pollAuth();
}

async function submitCode(){
    const code=document.getElementById('codeInput').value.trim();
    if(!code)return;
    await api('/api/auth/code',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code})});
    pollAuth();
}

async function doLogout(){
    await api('/api/auth/logout',{method:'POST'});
    showLogin();showMethods();
}

async function pollAuth(){
    const check=async()=>{
        const d=await api('/api/auth/state');
        if(d.error){
            showMethods();
            alert(d.error);
            return;
        }
        if(d.state==='ready'){
            showDash();loadAll();
            return;
        }
        if(d.state==='wait_code'){
            document.getElementById('loginMethods').classList.add('hidden');
            document.getElementById('phoneForm').classList.add('hidden');
            document.getElementById('qrForm').classList.add('hidden');
            document.getElementById('codeForm').classList.remove('hidden');
            document.getElementById('codeInput').focus();
            return;
        }
        if(d.state==='wait_qr'&&d.qr_link){
            renderQR(d.qr_link);
        }
        setTimeout(check,1500);
    };
    check();
}

function renderQR(link){
    const canvas=document.getElementById('qrCanvas');
    const s=200;canvas.width=s;canvas.height=s;
    const ctx=canvas.getContext('2d');
    const bg=getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()||'#fff';
    const fg=getComputedStyle(document.documentElement).getPropertyValue('--fg').trim()||'#000';
    try{
        const qr=qrcode(0,'L');qr.addData(link);qr.make();
        const n=qr.getModuleCount();const cell=Math.floor(s/n);const off=Math.floor((s-cell*n)/2);
        ctx.fillStyle=bg;ctx.fillRect(0,0,s,s);ctx.fillStyle=fg;
        for(let y=0;y<n;y++)for(let x=0;x<n;x++){if(qr.isDark(y,x))ctx.fillRect(off+x*cell,off+y*cell,cell,cell);}
    }catch(e){
        ctx.fillStyle=bg;ctx.fillRect(0,0,s,s);ctx.fillStyle=fg;ctx.font='11px monospace';ctx.textAlign='center';
        ctx.fillText('Scan link in Telegram:',s/2,s/2-10);ctx.fillText(link.substring(0,35),s/2,s/2+10);
    }
}

function formatSize(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';if(b<1073741824)return(b/1048576).toFixed(1)+' MB';return(b/1073741824).toFixed(1)+' GB';}

function buildTree(files,folders){
    const roots={};for(const wf of folders)roots[wf]={name:wf.split('/').pop(),path:wf,children:{},files:[]};
    for(const f of files){let root=null;for(const wf of folders){if(f.path.startsWith(wf+'/')){root=roots[wf];break;}}if(!root)continue;
        const rel=f.path.substring(root.path.length+1);const parts=rel.split('/');let node=root;
        for(let i=0;i<parts.length-1;i++){if(!node.children[parts[i]])node.children[parts[i]]={name:parts[i],children:{},files:[]};node=node.children[parts[i]];}
        node.files.push({name:parts[parts.length-1],size:f.size});}
    return roots;
}

function renderTree(roots){
    const el=document.getElementById('fileTree');let html='';
    for(const[wf,node]of Object.entries(roots))html+=renderNode(node,wf,true);
    el.innerHTML=html||'<div class="empty">No files yet.</div>';
}

function renderNode(node,fp,isRoot){
    const ck=Object.keys(node.children).sort();
    let h=`<div class="tree-dir" onclick="this.classList.toggle('open')"><span class="arrow">&#9654;</span><span class="name">${node.name}</span>`;
    if(isRoot)h+=`<button class="btn sm" style="margin-left:8px" onclick="event.stopPropagation();restoreFolder('${fp}')">Restore</button>`;
    h+=`</div><div class="tree-children">`;
    for(const k of ck)h+=renderNode(node.children[k],fp,false);
    for(const f of node.files.sort((a,b)=>a.name.localeCompare(b.name)))h+=`<div class="tree-file"><span class="name">${f.name}</span><span class="size">${formatSize(f.size)}</span></div>`;
    h+='</div>';return h;
}

document.getElementById('toggleFiles').onclick=function(){filesVisible=!filesVisible;document.getElementById('filesSection').classList.toggle('hidden',!filesVisible);this.textContent=filesVisible?'hide':'show';};

async function loadStatus(){try{const d=await api('/api/status');document.getElementById('statusDot').className=d.ready?'dot on':'dot';document.getElementById('statusText').textContent=d.ready?'synced':'connecting';const b=document.getElementById('queueBadge');if(d.queue>0){b.style.display='';b.textContent=d.queue+' queued';}else b.style.display='none';}catch(e){}}
async function loadWatched(){const d=await api('/api/folders');const p=document.getElementById('watchedPanel');if(!d.folders.length){p.innerHTML='<div class="empty">No folders added yet.</div>';return;}p.innerHTML=d.folders.map(f=>`<div class="row"><span class="path">${f}</span><div class="actions"><button class="btn sm" onclick="restoreFolder('${f}')">Restore</button><button class="btn danger sm" onclick="removeFolder('${f}')">Remove</button></div></div>`).join('');}
async function loadFiles(){const[fd,fr]=await Promise.all([api('/api/folders'),api('/api/files')]);renderTree(buildTree(fr.files,fd.folders));}

async function browse(path){currentPath=path;document.getElementById('currentPath').textContent=path;const d=await api('/api/browse?path='+encodeURIComponent(path));const el=document.getElementById('browser');let h='';
    if(path!=='/'){const par=path.split('/').slice(0,-1).join('/')||'/';h+=`<div class="item dir" onclick="browse('${par}')"><span class="icon">..</span></div>`;}
    for(const i of d.entries){if(i.is_dir)h+=`<div class="item dir" onclick="browse('${i.path}')"><span class="icon">/</span>${i.name}</div>`;else h+=`<div class="item file"><span class="icon">-</span>${i.name}</div>`;}
    el.innerHTML=h||'<div class="empty">Empty</div>';}

async function addFolder(p){await api('/api/folders',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:p})});await loadWatched();setTimeout(loadFiles,2000);}
async function removeFolder(p){await api('/api/folders',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:p})});await loadWatched();loadFiles();}

function setDlStatus(m){const el=document.getElementById('dlStatus');if(m){el.style.display='';el.textContent=m;}else el.style.display='none';}
async function restoreFolder(f){const d=document.getElementById('restorePath').value.trim();if(!d)return;setDlStatus('Restoring...');const r=await api('/api/restore/folder',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({source:f,dest:d})});setDlStatus((r.count||0)+' files restored');setTimeout(()=>setDlStatus(''),4000);}
async function restoreAll(){const d=document.getElementById('restorePath').value.trim();if(!d)return;setDlStatus('Restoring...');const r=await api('/api/restore/all',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({dest:d})});setDlStatus((r.count||0)+' files restored');setTimeout(()=>setDlStatus(''),4000);}

document.getElementById('addFolderBtn').onclick=()=>addFolder(currentPath);

function loadAll(){loadStatus();loadWatched();loadFiles();browse(currentPath);setInterval(loadStatus,3000);setInterval(loadFiles,15000);}

// Init: check auth state
(async()=>{
    const d=await api('/api/auth/state');
    if(d.state==='ready'){showDash();loadAll();}
    else{showLogin();}
})();
</script>
</body>
</html>
)html";

// --- main ---

int main() {
    DriveBackup backup;
    std::thread backup_thread([&backup]() { backup.run(); });

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);

    CROW_ROUTE(app, "/")([]() { crow::response r(HTML_PAGE); r.set_header("Content-Type","text/html"); return r; });

    // Auth endpoints
    CROW_ROUTE(app, "/api/auth/state")([&backup]() {
        crow::json::wvalue r;
        auto st = backup.auth_state();
        switch (st) {
        case DriveBackup::AuthState::WAIT_PHONE:
            r["state"] = "wait_phone";
            break;
        case DriveBackup::AuthState::WAIT_CODE:
            r["state"] = "wait_code";
            break;
        case DriveBackup::AuthState::WAIT_QR:
            r["state"] = "wait_qr";
            r["qr_link"] = backup.qr_link();
            break;
        case DriveBackup::AuthState::READY:
            r["state"] = "ready";
            break;
        case DriveBackup::AuthState::CLOSED:
            r["state"] = "closed";
            break;
        default:
            r["state"] = "none";
            break;
        }
        auto err = backup.auth_error();
        if (!err.empty())
            r["error"] = err;
        return r;
    });

    CROW_ROUTE(app, "/api/auth/phone").methods("POST"_method)([&backup](const crow::request &req) {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("phone"))
            return crow::response(400);
        backup.submit_phone(body["phone"].s());
        return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/auth/code").methods("POST"_method)([&backup](const crow::request &req) {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("code"))
            return crow::response(400);
        backup.submit_code(body["code"].s());
        return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/auth/qr").methods("POST"_method)([&backup]() {
        backup.request_qr_login();
        return crow::response(200, R"({"ok":true})");
    });

    CROW_ROUTE(app, "/api/auth/logout").methods("POST"_method)([&backup]() {
        backup.logout();
        return crow::response(200, R"({"ok":true})");
    });

    // Data endpoints
    CROW_ROUTE(app, "/api/status")([&backup]() { crow::json::wvalue r; r["ready"]=backup.ready(); r["queue"]=static_cast<int64_t>(backup.queue_size()); return r; });

    CROW_ROUTE(app, "/api/folders").methods("GET"_method)([&backup]() {
        auto f=backup.get_watched_folders(); crow::json::wvalue r; std::vector<crow::json::wvalue> a; for(auto&x:f)a.push_back(x); r["folders"]=std::move(a); return r; });

    CROW_ROUTE(app, "/api/folders").methods("POST"_method)([&backup](const crow::request &req) {
        auto b=crow::json::load(req.body); if(!b||!b.has("path"))return crow::response(400);
        if(backup.add_watched_folder(b["path"].s()))return crow::response(200,R"({"ok":true})"); return crow::response(400,R"({"error":"not a directory"})"); });

    CROW_ROUTE(app, "/api/folders").methods("DELETE"_method)([&backup](const crow::request &req) {
        auto b=crow::json::load(req.body); if(!b||!b.has("path"))return crow::response(400);
        backup.remove_watched_folder(b["path"].s()); return crow::response(200,R"({"ok":true})"); });

    CROW_ROUTE(app, "/api/files")([&backup]() {
        auto files=backup.get_backed_up_files(); crow::json::wvalue r; std::vector<crow::json::wvalue> a;
        for(auto&f:files){crow::json::wvalue i;i["message_id"]=f.message_id;i["size"]=f.file_size;i["path"]=f.file_path;i["modified_time"]=f.modified_time;a.push_back(std::move(i));}
        r["files"]=std::move(a); return r; });

    CROW_ROUTE(app, "/api/browse")([](const crow::request &req) {
        std::string path=req.url_params.get("path")?req.url_params.get("path"):"/home";
        crow::json::wvalue r; std::vector<crow::json::wvalue> entries; std::error_code ec;
        if(!fs::is_directory(path,ec)){r["entries"]=std::move(entries);return r;}
        std::vector<std::pair<std::string,bool>> sorted;
        for(auto&e:fs::directory_iterator(path,ec)){std::string n=e.path().filename().string();if(n.empty()||n[0]=='.')continue;sorted.emplace_back(e.path().string(),e.is_directory(ec));}
        std::sort(sorted.begin(),sorted.end(),[](auto&a,auto&b){if(a.second!=b.second)return a.second>b.second;return a.first<b.first;});
        for(auto&[fp,id]:sorted){crow::json::wvalue i;i["name"]=fs::path(fp).filename().string();i["path"]=fp;i["is_dir"]=id;entries.push_back(std::move(i));}
        r["entries"]=std::move(entries);return r; });

    CROW_ROUTE(app, "/api/restore/folder").methods("POST"_method)([&backup](const crow::request &req) {
        auto b=crow::json::load(req.body); if(!b||!b.has("source")||!b.has("dest"))return crow::response(400);
        int c=backup.restore_folder(b["source"].s(),b["dest"].s()); crow::json::wvalue r;r["ok"]=true;r["count"]=c;return crow::response(200,r.dump()); });

    CROW_ROUTE(app, "/api/restore/all").methods("POST"_method)([&backup](const crow::request &req) {
        auto b=crow::json::load(req.body); if(!b||!b.has("dest"))return crow::response(400);
        int c=backup.restore_all(b["dest"].s()); crow::json::wvalue r;r["ok"]=true;r["count"]=c;return crow::response(200,r.dump()); });

    std::cout << "[WEB] http://localhost:8080" << std::endl;
    app.port(8080).multithreaded().run();
    backup.stop();
    backup_thread.join();
    return 0;
}
