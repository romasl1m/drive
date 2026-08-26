#include "backup_engine.h"
#include "../config.h"

#include <iostream>
#include <filesystem>
#include <thread>
#include <algorithm>
#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace drive {

BackupEngine::BackupEngine() {
    td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
    client_manager_ = std::make_unique<td::ClientManager>();
    client_id_ = client_manager_->create_client_id();

    init_database();
    init_inotify();

    auto get_version = td::td_api::make_object<td::td_api::getOption>();
    get_version->name_ = "version";
    client_manager_->send(client_id_, 0, std::move(get_version));
}

BackupEngine::~BackupEngine() {
    if (db_) sqlite3_close(db_);
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

void BackupEngine::run() {
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

void BackupEngine::stop() { is_running_ = false; }

std::string BackupEngine::qr_link() {
    std::lock_guard<std::mutex> lock(auth_mtx_);
    return qr_link_;
}

std::string BackupEngine::auth_error() {
    std::lock_guard<std::mutex> lock(auth_mtx_);
    return auth_error_;
}

void BackupEngine::submit_phone(const std::string &phone) {
    {
        std::lock_guard<std::mutex> lock(auth_mtx_);
        auth_error_.clear();
    }
    auto r = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
    r->phone_number_ = phone;
    r->settings_ = td::td_api::make_object<td::td_api::phoneNumberAuthenticationSettings>();
    client_manager_->send(client_id_, 2, std::move(r));
}

void BackupEngine::submit_code(const std::string &code) {
    {
        std::lock_guard<std::mutex> lock(auth_mtx_);
        auth_error_.clear();
    }
    auto r = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
    r->code_ = code;
    client_manager_->send(client_id_, 3, std::move(r));
}

void BackupEngine::request_qr_login() {
    {
        std::lock_guard<std::mutex> lock(auth_mtx_);
        auth_error_.clear();
    }
    auto r = td::td_api::make_object<td::td_api::requestQrCodeAuthentication>();
    client_manager_->send(client_id_, 2, std::move(r));
}

void BackupEngine::logout() {
    client_manager_->send(client_id_, 300, td::td_api::make_object<td::td_api::logOut>());
}

// --- Folders ---

std::vector<std::string> BackupEngine::get_watched_folders() {
    std::lock_guard<std::mutex> lock(db_mtx_);
    return db_get_watched_folders();
}

bool BackupEngine::add_watched_folder(const std::string &path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return false;
    {
        std::lock_guard<std::mutex> lock(db_mtx_);
        sqlite3_stmt *stmt;
        const char *sql = "INSERT OR IGNORE INTO watched_folders (folder_path) VALUES (?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    add_watch_recursive(path);
    if (is_ready_) scan_directory(path);
    if (progress_cb_) progress_cb_();
    return true;
}

bool BackupEngine::remove_watched_folder(const std::string &path) {
    std::lock_guard<std::mutex> lock(db_mtx_);
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM watched_folders WHERE folder_path = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    remove_watches_under(path);
    if (progress_cb_) progress_cb_();
    return true;
}

// --- Files ---

std::vector<BackupEngine::FileRecord> BackupEngine::get_backed_up_files() {
    std::lock_guard<std::mutex> lock(db_mtx_);
    std::vector<FileRecord> result;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT message_id, file_size, file_path, modified_time FROM files ORDER BY file_path;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
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

std::vector<BackupEngine::FileRecord> BackupEngine::get_files_under(const std::string &prefix) {
    std::lock_guard<std::mutex> lock(db_mtx_);
    std::vector<FileRecord> result;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT message_id, file_size, file_path, modified_time FROM files WHERE file_path LIKE ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
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

BackupEngine::Stats BackupEngine::get_stats() {
    std::lock_guard<std::mutex> lock(db_mtx_);
    Stats s;
    auto folders = db_get_watched_folders();
    sqlite3_stmt *stmt;
    const char *sql = "SELECT file_size, file_path, modified_time FROM files;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return s;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::int64_t size = sqlite3_column_int64(stmt, 0);
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        std::string path = p ? p : "";
        s.total_files++;
        s.total_size += size;
        if (size > s.largest_file_size) {
            s.largest_file_size = size;
            s.largest_file_path = path;
        }
        std::string ext;
        auto dot = path.rfind('.');
        auto slash = path.rfind('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            ext = path.substr(dot + 1);
        if (ext.empty()) ext = "other";
        for (auto &c : ext) c = static_cast<char>(std::tolower(c));
        s.type_count[ext]++;
        s.type_size[ext] += size;
        for (auto &f : folders) {
            if (path.rfind(f, 0) == 0) {
                s.folder_size[f] += size;
                s.folder_count[f]++;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    return s;
}

// --- Restore ---

int BackupEngine::restore_folder(const std::string &source_folder, const std::string &dest) {
    auto files = get_files_under(source_folder);
    if (files.empty()) return 0;
    std::error_code ec;
    fs::create_directories(dest, ec);
    int count = 0;
    for (auto &f : files) {
        std::string rel = f.file_path.substr(source_folder.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        std::string dest_path = dest + "/" + rel;
        fs::create_directories(fs::path(dest_path).parent_path(), ec);
        if (fs::exists(f.file_path, ec)) {
            fs::copy_file(f.file_path, dest_path, fs::copy_options::overwrite_existing, ec);
            if (!ec) count++;
        } else {
            std::string dl = request_download(f.message_id);
            if (!dl.empty()) {
                fs::copy_file(dl, dest_path, fs::copy_options::overwrite_existing, ec);
                if (!ec) count++;
            }
        }
    }
    return count;
}

int BackupEngine::restore_all(const std::string &dest) {
    auto folders = get_watched_folders();
    int total = 0;
    for (auto &folder : folders) {
        total += restore_folder(folder, dest + "/" + fs::path(folder).filename().string());
    }
    return total;
}

// --- Private: Init ---

void BackupEngine::init_database() {
    std::string dir = data_dir();
    fs::create_directories(dir);
    std::string db_path = dir + "/backup.db";
    sqlite3_open(db_path.c_str(), &db_);
    sqlite3_exec(db_,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS channels(user_id INTEGER PRIMARY KEY, chat_id INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS files(message_id INTEGER PRIMARY KEY, file_size INTEGER NOT NULL, file_path TEXT NOT NULL UNIQUE, modified_time INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS watched_folders(folder_path TEXT PRIMARY KEY);",
        nullptr, nullptr, nullptr);
}

void BackupEngine::init_inotify() {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        std::cerr << "[Drive] inotify init failed" << std::endl;
    }
}

// --- Private: Inotify ---

void BackupEngine::add_watch_recursive(const std::string &dir) {
    int wd = inotify_add_watch(inotify_fd_, dir.c_str(),
        IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
    if (wd < 0) return;
    wd_to_path_[wd] = dir;
    std::error_code ec;
    for (auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) add_watch_recursive(entry.path().string());
    }
}

void BackupEngine::remove_watches_under(const std::string &prefix) {
    std::vector<int> rm;
    for (auto &[wd, path] : wd_to_path_) {
        if (path == prefix || path.rfind(prefix + "/", 0) == 0) {
            inotify_rm_watch(inotify_fd_, wd);
            rm.push_back(wd);
        }
    }
    for (int w : rm) wd_to_path_.erase(w);
}

void BackupEngine::poll_inotify() {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (true) {
        ssize_t len = read(inotify_fd_, buf, sizeof(buf));
        if (len <= 0) break;
        char *ptr = buf;
        while (ptr < buf + len) {
            auto *ev = reinterpret_cast<struct inotify_event *>(ptr);
            handle_inotify_event(ev);
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }
}

void BackupEngine::handle_inotify_event(struct inotify_event *ev) {
    if (ev->mask & IN_ISDIR) {
        if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
            std::string d = wd_to_path_[ev->wd] + "/" + ev->name;
            add_watch_recursive(d);
            scan_directory(d);
        }
        return;
    }
    if (!ev->len) return;
    std::string fp = wd_to_path_[ev->wd] + "/" + ev->name;
    if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
        schedule_file_sync(fp);
    else if (ev->mask & (IN_DELETE | IN_MOVED_FROM))
        schedule_file_delete(fp);
}

// --- Private: Sync ---

void BackupEngine::schedule_file_sync(const std::string &path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    std::int64_t size = static_cast<std::int64_t>(fs::file_size(path, ec));
    auto ft = fs::last_write_time(path, ec);
    std::int64_t mtime = std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
    std::int64_t mid = db_get_file_message(path);
    if (mid != 0) {
        if (db_file_unchanged(path, size, mtime)) return;
        sync_queue_.push({SyncAction::UPDATE, path, mid});
    } else {
        sync_queue_.push({SyncAction::UPLOAD, path, 0});
    }
}

void BackupEngine::schedule_file_delete(const std::string &path) {
    std::int64_t mid = db_get_file_message(path);
    if (mid == 0) return;
    sync_queue_.push({SyncAction::DELETE, path, mid});
}

void BackupEngine::process_queue() {
    if (sync_queue_.empty() || is_busy_) return;
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
    if (progress_cb_) progress_cb_();
}

void BackupEngine::perform_initial_scan() {
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
            bool watched = false;
            for (auto &f : folders) {
                if (path.rfind(f, 0) == 0) { watched = true; break; }
            }
            if (watched)
                schedule_file_delete(path);
            else {
                std::lock_guard<std::mutex> lock(db_mtx_);
                db_remove_file_by_path(path);
            }
        }
    }
    if (progress_cb_) progress_cb_();
}

void BackupEngine::scan_directory(const std::string &dir) {
    std::error_code ec;
    for (auto &e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) schedule_file_sync(e.path().string());
    }
}

void BackupEngine::send_file(const std::string &fp) {
    auto req = td::td_api::make_object<td::td_api::sendMessage>();
    req->chat_id_ = chat_id_;
    req->input_message_content_ = td::td_api::make_object<td::td_api::inputMessageDocument>(
        td::td_api::make_object<td::td_api::inputDocument>(
            td::td_api::make_object<td::td_api::inputFileLocal>(fp), nullptr, false),
        td::td_api::make_object<td::td_api::formattedText>());
    client_manager_->send(client_id_, 100, std::move(req));
}

void BackupEngine::delete_message(std::int64_t mid) {
    auto req = td::td_api::make_object<td::td_api::deleteMessages>();
    req->chat_id_ = chat_id_;
    req->message_ids_ = {mid};
    req->revoke_ = true;
    client_manager_->send(client_id_, 101, std::move(req));
}

// --- Private: DB ---

std::vector<std::string> BackupEngine::db_get_watched_folders() {
    std::vector<std::string> r;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, "SELECT folder_path FROM watched_folders ORDER BY folder_path;", -1, &stmt, nullptr) != SQLITE_OK)
        return r;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (p) r.emplace_back(p);
    }
    sqlite3_finalize(stmt);
    return r;
}

void BackupEngine::db_save_file(std::int64_t mid, std::int64_t sz, const std::string &fp, std::int64_t mt) {
    std::lock_guard<std::mutex> lock(db_mtx_);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO files(message_id,file_size,file_path,modified_time)VALUES(?,?,?,?)", -1, &s, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, mid);
    sqlite3_bind_int64(s, 2, sz);
    sqlite3_bind_text(s, 3, fp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, mt);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void BackupEngine::db_remove_file_by_path(const std::string &p) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "DELETE FROM files WHERE file_path=?", -1, &s, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

std::int64_t BackupEngine::db_get_file_message(const std::string &p) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "SELECT message_id FROM files WHERE file_path=?", -1, &s, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
    std::int64_t r = 0;
    if (sqlite3_step(s) == SQLITE_ROW) r = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return r;
}

bool BackupEngine::db_file_unchanged(const std::string &p, std::int64_t sz, std::int64_t mt) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM files WHERE file_path=? AND file_size=? AND modified_time=?", -1, &s, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, sz);
    sqlite3_bind_int64(s, 3, mt);
    bool r = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    return r;
}

std::vector<std::pair<std::string, std::int64_t>> BackupEngine::db_get_all_files() {
    std::vector<std::pair<std::string, std::int64_t>> r;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "SELECT file_path,message_id FROM files", -1, &s, nullptr) != SQLITE_OK) return r;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
        if (p) r.emplace_back(p, sqlite3_column_int64(s, 1));
    }
    sqlite3_finalize(s);
    return r;
}

std::int64_t BackupEngine::db_get_channel(std::int64_t uid) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "SELECT chat_id FROM channels WHERE user_id=?", -1, &s, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(s, 1, uid);
    std::int64_t r = 0;
    if (sqlite3_step(s) == SQLITE_ROW) r = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return r;
}

void BackupEngine::db_save_channel(std::int64_t uid, std::int64_t cid) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO channels(user_id,chat_id)VALUES(?,?)", -1, &s, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_int64(s, 2, cid);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// --- Private: Download ---

std::string BackupEngine::request_download(std::int64_t message_id) {
    std::int32_t file_id = get_file_id_for_message(message_id);
    if (file_id == 0) return "";
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

std::int32_t BackupEngine::get_file_id_for_message(std::int64_t message_id) {
    if (!history_loaded_) load_chat_history_sync();
    auto it = msg_to_file_id_.find(message_id);
    if (it != msg_to_file_id_.end()) return it->second;
    load_chat_history_sync();
    it = msg_to_file_id_.find(message_id);
    return (it != msg_to_file_id_.end()) ? it->second : 0;
}

void BackupEngine::load_chat_history_sync() {
    auto request = td::td_api::make_object<td::td_api::getChatHistory>();
    request->chat_id_ = chat_id_;
    request->from_message_id_ = 0;
    request->offset_ = 0;
    request->limit_ = 100;
    request->only_local_ = false;
    client_manager_->send(client_id_, 201, std::move(request));
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

// --- Private: TDLib responses ---

void BackupEngine::process_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj) {
    if (!obj) return;
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
        if (progress_cb_) progress_cb_();
    } else if (id == td::td_api::ok::ID && rid == 101) {
        {
            std::lock_guard<std::mutex> lock(db_mtx_);
            db_remove_file_by_path(current_task_.file_path);
        }
        if (current_task_.action == SyncAction::UPDATE)
            send_file(current_task_.file_path);
        else {
            is_busy_ = false;
            if (progress_cb_) progress_cb_();
        }
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
            if (!m || !m->content_) continue;
            auto cid = m->content_->get_id();
            if (cid == td::td_api::messageDocument::ID) {
                auto *d = static_cast<td::td_api::messageDocument *>(m->content_.get());
                if (d->document_) msg_to_file_id_[m->id_] = d->document_->document_->id_;
            } else if (cid == td::td_api::messagePhoto::ID) {
                auto *p = static_cast<td::td_api::messagePhoto *>(m->content_.get());
                if (p->photo_ && !p->photo_->sizes_.empty())
                    msg_to_file_id_[m->id_] = p->photo_->sizes_.back()->photo_->id_;
            } else if (cid == td::td_api::messageVideo::ID) {
                auto *v = static_cast<td::td_api::messageVideo *>(m->content_.get());
                if (v->video_) msg_to_file_id_[m->id_] = v->video_->video_->id_;
            }
        }
        history_loaded_ = true;
    } else if (id == td::td_api::error::ID) {
        auto e = td::move_tl_object_as<td::td_api::error>(obj);
        if (rid == 2 || rid == 3) {
            std::lock_guard<std::mutex> lock(auth_mtx_);
            auth_error_ = e->message_;
            if (error_cb_) error_cb_(auth_error_);
        } else if (rid == 200) {
            std::lock_guard<std::mutex> lock(dl_mtx_);
            dl_completed_ = true;
            dl_cv_.notify_all();
        } else if (rid == 13) {
            search_backup_channel();
        } else if (rid == 100 || rid == 101) {
            is_busy_ = false;
        }
    }
}

void BackupEngine::handle_auth_state(td::td_api::object_ptr<td::td_api::AuthorizationState> state) {
    if (!state) return;
    switch (state->get_id()) {
    case td::td_api::authorizationStateWaitTdlibParameters::ID: {
        auto r = td::td_api::make_object<td::td_api::setTdlibParameters>();
        r->database_directory_ = data_dir() + "/tdlib";
        r->use_message_database_ = true;
        r->use_secret_chats_ = true;
        r->api_id_ = APP_API_ID;
        r->api_hash_ = APP_API_HASH;
        r->system_language_code_ = "en";
        r->device_model_ = "Drive Desktop";
        r->application_version_ = APP_VERSION;
        client_manager_->send(client_id_, 1, std::move(r));
        break;
    }
    case td::td_api::authorizationStateWaitPhoneNumber::ID:
        auth_state_ = AuthState::WAIT_PHONE;
        if (auth_cb_) auth_cb_(AuthState::WAIT_PHONE);
        break;
    case td::td_api::authorizationStateWaitCode::ID:
        auth_state_ = AuthState::WAIT_CODE;
        if (auth_cb_) auth_cb_(AuthState::WAIT_CODE);
        break;
    case td::td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
        auto s = td::move_tl_object_as<td::td_api::authorizationStateWaitOtherDeviceConfirmation>(state);
        {
            std::lock_guard<std::mutex> lock(auth_mtx_);
            qr_link_ = s->link_;
        }
        auth_state_ = AuthState::WAIT_QR;
        if (auth_cb_) auth_cb_(AuthState::WAIT_QR);
        break;
    }
    case td::td_api::authorizationStateReady::ID:
        auth_state_ = AuthState::READY;
        if (auth_cb_) auth_cb_(AuthState::READY);
        client_manager_->send(client_id_, 12, td::td_api::make_object<td::td_api::getMe>());
        break;
    case td::td_api::authorizationStateClosed::ID:
        auth_state_ = AuthState::CLOSED;
        is_ready_ = false;
        if (auth_cb_) auth_cb_(AuthState::CLOSED);
        break;
    default:
        break;
    }
}

void BackupEngine::handle_chat_response(std::int64_t rid, td::td_api::object_ptr<td::td_api::Object> obj) {
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

void BackupEngine::resolve_backup_channel() {
    auto id = db_get_channel(user_id_);
    if (id) {
        auto r = td::td_api::make_object<td::td_api::getChat>();
        r->chat_id_ = id;
        client_manager_->send(client_id_, 13, std::move(r));
    } else {
        search_backup_channel();
    }
}

void BackupEngine::search_backup_channel() {
    auto r = td::td_api::make_object<td::td_api::searchChatsOnServer>();
    r->query_ = "backup";
    r->limit_ = 20;
    client_manager_->send(client_id_, 10, std::move(r));
}

void BackupEngine::check_next_candidate() {
    if (search_index_ >= search_chat_ids_.size()) {
        auto r = td::td_api::make_object<td::td_api::createNewSupergroupChat>();
        r->title_ = "backup";
        r->is_channel_ = true;
        r->description_ = "Drive backup storage";
        client_manager_->send(client_id_, 4, std::move(r));
        return;
    }
    auto r = td::td_api::make_object<td::td_api::getChat>();
    r->chat_id_ = search_chat_ids_[search_index_];
    client_manager_->send(client_id_, 11, std::move(r));
}

void BackupEngine::start_watching() {
    is_ready_ = true;
    if (auth_cb_) auth_cb_(AuthState::READY);
    if (progress_cb_) progress_cb_();
    perform_initial_scan();
}

} // namespace drive
