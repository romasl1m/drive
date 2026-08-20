#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>
#include <vector>
#include <filesystem>
#include <sqlite3.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

class TelegramAuth {
  public:
    TelegramAuth() {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        client_manager_ = std::make_unique<td::ClientManager>();
        client_id_ = client_manager_->create_client_id();
        std::cout << "[INFO] Inicjalizacja klienta TDLib (ID: " << client_id_ << ")..." << std::endl;

        init_database();

        auto get_version = td::td_api::make_object<td::td_api::getOption>();
        get_version->name_ = "version";
        client_manager_->send(client_id_, 0, std::move(get_version));
    }

    ~TelegramAuth() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    void run() {
        std::cout << "[INFO] Start pętli zdarzeń." << std::endl;
        while (is_running_) {
            auto response = client_manager_->receive(1.0);
            if (response.object != nullptr) {
                process_response(response.request_id, std::move(response.object));
            }
        }
        std::cout << "[INFO] Koniec pętli zdarzeń." << std::endl;
    }

  private:
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_ = 0;
    bool is_authorized_ = false;
    bool is_running_ = true;
    std::int64_t created_chat_id_ = 0;
    std::int64_t user_id_ = 0;

    sqlite3 *db_ = nullptr;

    std::vector<std::int64_t> search_chat_ids_;
    std::size_t search_index_ = 0;

    struct FileInfo {
        std::int64_t message_id;
        std::int32_t file_id;
        std::string file_name;
        std::int64_t file_size;
    };
    std::vector<FileInfo> found_files_;

    bool is_downloading_ = false;
    std::int32_t downloading_file_id_ = 0;
    std::string download_dir_ = "/home/roman/Downloads/drive";

    void move_to_downloads(const std::string &src_path) {
        namespace fs = std::filesystem;
        fs::create_directories(download_dir_);
        fs::path src(src_path);
        fs::path dst = fs::path(download_dir_) / src.filename();
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[BŁĄD] Nie można skopiować pliku do " << dst << ": " << ec.message() << std::endl;
        } else {
            std::cout << "[INFO] Plik zapisany do: " << dst.string() << std::endl;
        }
    }

    void init_database() {
        int rc = sqlite3_open("backup.db", &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "[BŁĄD] Nie można otworzyć bazy danych: " << sqlite3_errmsg(db_) << std::endl;
            exit(1);
        }

        const char *sql = "CREATE TABLE IF NOT EXISTS channels ("
                          "user_id INTEGER PRIMARY KEY, "
                          "chat_id INTEGER NOT NULL);";
        char *err_msg = nullptr;
        rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "[BŁĄD] Nie można utworzyć tabeli: " << err_msg << std::endl;
            sqlite3_free(err_msg);
            exit(1);
        }
    }

    std::int64_t db_get_channel(std::int64_t user_id) {
        sqlite3_stmt *stmt;
        const char *sql = "SELECT chat_id FROM channels WHERE user_id = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_int64(stmt, 1, user_id);
        std::int64_t result = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    void db_save_channel(std::int64_t user_id, std::int64_t chat_id) {
        sqlite3_stmt *stmt;
        const char *sql = "INSERT OR REPLACE INTO channels (user_id, chat_id) VALUES (?, ?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[BŁĄD] Nie można zapisać kanału: " << sqlite3_errmsg(db_) << std::endl;
            return;
        }
        sqlite3_bind_int64(stmt, 1, user_id);
        sqlite3_bind_int64(stmt, 2, chat_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void process_response(std::int64_t request_id, td::td_api::object_ptr<td::td_api::Object> object) {
        if (not object) {
            return;
        }

        std::int32_t id = object->get_id();

        if (id == td::td_api::updateAuthorizationState::ID) {
            auto auth_state = td::move_tl_object_as<td::td_api::updateAuthorizationState>(object);
            handle_auth_state(std::move(auth_state->authorization_state_));
        } else if (id == td::td_api::user::ID) {
            if (request_id == 12) {
                auto user = td::move_tl_object_as<td::td_api::user>(object);
                user_id_ = user->id_;
                std::cout << "[INFO] Zalogowano jako user ID: " << user_id_ << std::endl;
                resolve_backup_channel();
            }
        } else if (id == td::td_api::chats::ID) {
            if (request_id == 10) {
                auto chats = td::move_tl_object_as<td::td_api::chats>(object);
                search_chat_ids_ = std::move(chats->chat_ids_);
                search_index_ = 0;
                check_next_candidate();
            }
        } else if (id == td::td_api::chat::ID) {
            auto chat = td::move_tl_object_as<td::td_api::chat>(object);

            if (request_id == 13) {
                created_chat_id_ = chat->id_;
                std::cout << "\n[Sukces] Kanał backup załadowany z bazy! ID: " << created_chat_id_ << std::endl;
                show_menu();
            } else if (request_id == 11) {
                if (chat->title_ == "backup" && chat->type_->get_id() == td::td_api::chatTypeSupergroup::ID) {
                    auto *sg = static_cast<td::td_api::chatTypeSupergroup *>(chat->type_.get());
                    if (sg->is_channel_) {
                        created_chat_id_ = chat->id_;
                        db_save_channel(user_id_, created_chat_id_);
                        std::cout << "\n[Sukces] Znaleziono istniejący kanał backup! ID: " << created_chat_id_ << std::endl;
                        show_menu();
                        return;
                    }
                }
                search_index_++;
                check_next_candidate();
            } else if (request_id == 4) {
                created_chat_id_ = chat->id_;
                db_save_channel(user_id_, created_chat_id_);
                std::cout << "\n[Sukces] Kanał został utworzony! ID: " << created_chat_id_ << std::endl;
                send_first_message(created_chat_id_, "Kanał backup zainicjalizowany.");
            }
        } else if (id == td::td_api::message::ID) {
            if (request_id == 5) {
                auto message = td::move_tl_object_as<td::td_api::message>(object);
                std::cout << "[Sukces] Wiadomość wysłana! ID: " << message->id_ << std::endl;
                get_invite_link(created_chat_id_);
            } else if (request_id == 7) {
                auto message = td::move_tl_object_as<td::td_api::message>(object);
                std::cout << "[Sukces] Plik wysłany! ID wiadomości: " << message->id_ << std::endl;
                show_menu();
            }
        } else if (id == td::td_api::chatInviteLink::ID) {
            if (request_id == 6) {
                auto invite_link = td::move_tl_object_as<td::td_api::chatInviteLink>(object);
                std::cout << "\nPrywatny link zaproszenia: " << invite_link->invite_link_ << std::endl;
                show_menu();
            }
        } else if (id == td::td_api::messages::ID) {
            if (request_id == 8) {
                handle_chat_history(std::move(object));
            }
        } else if (id == td::td_api::file::ID) {
            if (request_id == 9) {
                auto file = td::move_tl_object_as<td::td_api::file>(object);
                handle_file_downloaded(std::move(file));
            }
        } else if (id == td::td_api::updateFile::ID) {
            auto update = td::move_tl_object_as<td::td_api::updateFile>(object);
            if (update->file_) {
                std::cout << "[DEBUG] updateFile: file_id=" << update->file_->id_
                          << ", size=" << update->file_->size_ << std::endl;
                if (update->file_->local_) {
                    std::cout << "[DEBUG]   local: downloaded_size=" << update->file_->local_->downloaded_size_
                              << ", is_downloading_active=" << update->file_->local_->is_downloading_active_
                              << ", is_downloading_completed=" << update->file_->local_->is_downloading_completed_
                              << ", path=" << update->file_->local_->path_ << std::endl;
                }

                if (is_downloading_ && update->file_->id_ == downloading_file_id_) {
                    if (update->file_->local_ && update->file_->local_->is_downloading_completed_) {
                        std::cout << "[Sukces] Pobrano plik: " << update->file_->local_->path_ << std::endl;
                        move_to_downloads(update->file_->local_->path_);
                        is_downloading_ = false;
                        downloading_file_id_ = 0;
                        show_menu();
                    } else if (update->file_->local_ && update->file_->local_->is_downloading_active_) {
                        std::int64_t downloaded = update->file_->local_->downloaded_size_;
                        std::int64_t total = update->file_->size_;
                        if (total > 0) {
                            int percent = static_cast<int>(100 * downloaded / total);
                            std::cout << "[Postęp] " << downloaded << " / " << total
                                      << " bajtów (" << percent << "%)" << std::endl;
                        } else {
                            std::cout << "[Postęp] " << downloaded << " bajtów pobrano" << std::endl;
                        }
                    }
                }
            }
        } else if (id == td::td_api::error::ID) {
            auto error = td::move_tl_object_as<td::td_api::error>(object);
            std::cerr << "\n[BŁĄD TELEGRAMA] Request ID: " << request_id
                      << " | Kod: " << error->code_
                      << " | Wiadomość: " << error->message_ << std::endl;
            if (request_id == 9) {
                std::cerr << "[DEBUG] Błąd pobierania pliku! file_id=" << downloading_file_id_ << std::endl;
                is_downloading_ = false;
                downloading_file_id_ = 0;
            }
            if (request_id == 13) {
                std::cout << "[INFO] Kanał z bazy nie istnieje. Szukanie..." << std::endl;
                search_backup_channel();
            } else if (is_authorized_ && created_chat_id_ != 0) {
                show_menu();
            } else {
                is_running_ = false;
            }
        }
    }

    void handle_auth_state(td::td_api::object_ptr<td::td_api::AuthorizationState> state) {
        if (not state) {
            return;
        }

        switch (state->get_id()) {
        case td::td_api::authorizationStateWaitTdlibParameters::ID: {
            std::cout << "[STAN] Otrzymano żądanie parametrów TDLib..." << std::endl;

            const char *raw_api_id = std::getenv("API_ID");
            const char *raw_api_hash = std::getenv("API_HASH");

            if (not raw_api_id or not raw_api_hash) {
                std::cerr << "[BŁĄD] Brak zmiennych środowiskowych API_ID lub API_HASH!" << std::endl;
                std::cerr << "Uruchom program wpisując najpierw w terminalu:" << std::endl;
                std::cerr << "export API_ID=twój_id" << std::endl;
                std::cerr << "export API_HASH=twój_hash" << std::endl;
                exit(1);
            }

            auto request = td::td_api::make_object<td::td_api::setTdlibParameters>();
            request->database_directory_ = "tdlib";
            request->use_message_database_ = true;
            request->use_secret_chats_ = true;
            request->api_id_ = std::stoi(raw_api_id);
            request->api_hash_ = raw_api_hash;
            request->system_language_code_ = "pl";
            request->device_model_ = "Desktop";
            request->application_version_ = "1.0";

            std::cout << "[INFO] Wysyłanie parametrów do TDLib..." << std::endl;
            client_manager_->send(client_id_, 1, std::move(request));
            break;
        }
        case td::td_api::authorizationStateWaitPhoneNumber::ID: {
            std::cout << "[STAN] Oczekiwanie na numer telefonu." << std::endl;
            std::cout << "Podaj numer telefonu (z kierunkowym, np. +48...): " << std::flush;
            std::string phone;
            std::cin >> phone;

            auto request = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
            request->phone_number_ = phone;
            client_manager_->send(client_id_, 2, std::move(request));
            break;
        }
        case td::td_api::authorizationStateWaitCode::ID: {
            std::cout << "[STAN] Oczekiwanie na kod autoryzacyjny." << std::endl;
            std::cout << "Podaj kod z aplikacji/SMS: " << std::flush;
            std::string code;
            std::cin >> code;

            auto request = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
            request->code_ = code;
            client_manager_->send(client_id_, 3, std::move(request));
            break;
        }
        case td::td_api::authorizationStateReady::ID: {
            if (not is_authorized_) {
                is_authorized_ = true;
                std::cout << "\n[STAN] Zalogowano pomyślnie!" << std::endl;
                auto request = td::td_api::make_object<td::td_api::getMe>();
                client_manager_->send(client_id_, 12, std::move(request));
            }
            break;
        }
        case td::td_api::authorizationStateClosed::ID: {
            std::cout << "[STAN] Sesja zamknięta." << std::endl;
            is_running_ = false;
            break;
        }
        default:
            std::cout << "[STAN] Inny stan autoryzacji ID: " << state->get_id() << std::endl;
            break;
        }
    }

    void resolve_backup_channel() {
        std::int64_t stored_id = db_get_channel(user_id_);
        if (stored_id != 0) {
            std::cout << "[INFO] Znaleziono kanał w bazie (ID: " << stored_id << "). Weryfikacja..." << std::endl;
            auto request = td::td_api::make_object<td::td_api::getChat>();
            request->chat_id_ = stored_id;
            client_manager_->send(client_id_, 13, std::move(request));
        } else {
            std::cout << "[INFO] Brak kanału w bazie. Szukanie na serwerze..." << std::endl;
            search_backup_channel();
        }
    }

    void show_menu() {
        std::cout << "\n===== MENU =====" << std::endl;
        std::cout << "1. Wyślij plik na kanał" << std::endl;
        std::cout << "2. Wyświetl pliki na kanale" << std::endl;
        std::cout << "3. Pobierz plik z kanału" << std::endl;
        std::cout << "4. Wyjście" << std::endl;
        std::cout << "Wybierz opcję: " << std::flush;

        std::string choice;
        std::cin >> choice;

        if (choice == "1") {
            std::cout << "Podaj ścieżkę do pliku: " << std::flush;
            std::string path;
            std::cin.ignore();
            std::getline(std::cin, path);
            send_file(created_chat_id_, path);
        } else if (choice == "2") {
            list_files(created_chat_id_);
        } else if (choice == "3") {
            if (found_files_.empty()) {
                std::cout << "[INFO] Najpierw wyświetl pliki (opcja 2), aby zobaczyć dostępne pliki." << std::endl;
                show_menu();
            } else {
                std::cout << "Podaj numer pliku do pobrania (1-" << found_files_.size() << "): " << std::flush;
                int file_num;
                std::cin >> file_num;
                if (file_num >= 1 && file_num <= static_cast<int>(found_files_.size())) {
                    download_file(found_files_[file_num - 1].file_id);
                } else {
                    std::cout << "[BŁĄD] Nieprawidłowy numer." << std::endl;
                    show_menu();
                }
            }
        } else if (choice == "4") {
            is_running_ = false;
        } else {
            std::cout << "[BŁĄD] Nieprawidłowa opcja." << std::endl;
            show_menu();
        }
    }

    void search_backup_channel() {
        auto request = td::td_api::make_object<td::td_api::searchChatsOnServer>();
        request->query_ = "backup";
        request->limit_ = 20;
        client_manager_->send(client_id_, 10, std::move(request));
    }

    void check_next_candidate() {
        if (search_index_ >= search_chat_ids_.size()) {
            std::cout << "[INFO] Nie znaleziono kanału backup. Tworzenie nowego..." << std::endl;
            create_private_channel();
            return;
        }
        auto request = td::td_api::make_object<td::td_api::getChat>();
        request->chat_id_ = search_chat_ids_[search_index_];
        client_manager_->send(client_id_, 11, std::move(request));
    }

    void create_private_channel() {
        auto request = td::td_api::make_object<td::td_api::createNewSupergroupChat>();
        request->title_ = "backup";
        request->is_channel_ = true;
        request->description_ = "Kanał testowy utworzony przez C++";

        client_manager_->send(client_id_, 4, std::move(request));
    }

    void send_first_message(std::int64_t chat_id, const std::string &text) {
        auto request = td::td_api::make_object<td::td_api::sendMessage>();
        request->chat_id_ = chat_id;

        auto content = td::td_api::make_object<td::td_api::inputMessageText>();
        auto formatted_text = td::td_api::make_object<td::td_api::formattedText>();
        formatted_text->text_ = text;
        content->text_ = std::move(formatted_text);

        request->input_message_content_ = std::move(content);
        client_manager_->send(client_id_, 5, std::move(request));
    }

    void send_file(std::int64_t chat_id, const std::string &file_path) {
        auto request = td::td_api::make_object<td::td_api::sendMessage>();
        request->chat_id_ = chat_id;

        auto content = td::td_api::make_object<td::td_api::inputMessageDocument>(
            td::td_api::make_object<td::td_api::inputDocument>(
                td::td_api::make_object<td::td_api::inputFileLocal>(file_path),
                nullptr,
                false),
            td::td_api::make_object<td::td_api::formattedText>());

        request->input_message_content_ = std::move(content);

        std::cout << "[INFO] Wysyłanie pliku: " << file_path << std::endl;
        client_manager_->send(client_id_, 7, std::move(request));
    }

    void get_invite_link(std::int64_t chat_id) {
        auto request = td::td_api::make_object<td::td_api::getChatInviteLink>();
        request->chat_id_ = chat_id;

        client_manager_->send(client_id_, 6, std::move(request));
    }

    void list_files(std::int64_t chat_id) {
        auto request = td::td_api::make_object<td::td_api::getChatHistory>();
        request->chat_id_ = chat_id;
        request->from_message_id_ = 0;
        request->offset_ = 0;
        request->limit_ = 100;
        request->only_local_ = false;

        std::cout << "[INFO] Pobieranie historii wiadomości..." << std::endl;
        client_manager_->send(client_id_, 8, std::move(request));
    }

    void handle_chat_history(td::td_api::object_ptr<td::td_api::Object> object) {
        auto messages = td::move_tl_object_as<td::td_api::messages>(object);
        found_files_.clear();

        std::cout << "[DEBUG] handle_chat_history: total_count=" << messages->total_count_
                  << ", messages.size()=" << messages->messages_.size() << std::endl;

        std::cout << "\n===== PLIKI NA KANALE =====" << std::endl;
        int index = 0;

        for (auto &msg : messages->messages_) {
            if (!msg || !msg->content_)
                continue;

            std::int32_t content_id = msg->content_->get_id();

            if (content_id == td::td_api::messageDocument::ID) {
                auto *doc_msg = static_cast<td::td_api::messageDocument *>(msg->content_.get());
                if (doc_msg->document_) {
                    index++;
                    FileInfo info;
                    info.message_id = msg->id_;
                    info.file_id = doc_msg->document_->document_->id_;
                    info.file_name = doc_msg->document_->file_name_;
                    info.file_size = doc_msg->document_->document_->size_;
                    found_files_.push_back(info);
                    std::cout << "[DEBUG] Dokument: file_id=" << info.file_id
                              << ", msg_id=" << info.message_id
                              << ", remote_id=" << doc_msg->document_->document_->remote_->id_ << std::endl;

                    std::cout << "  " << index << ". " << info.file_name
                              << " (" << info.file_size << " bajtów)" << std::endl;
                }
            } else if (content_id == td::td_api::messagePhoto::ID) {
                auto *photo_msg = static_cast<td::td_api::messagePhoto *>(msg->content_.get());
                if (photo_msg->photo_ && !photo_msg->photo_->sizes_.empty()) {
                    auto &largest = photo_msg->photo_->sizes_.back();
                    index++;
                    FileInfo info;
                    info.message_id = msg->id_;
                    info.file_id = largest->photo_->id_;
                    info.file_name = "photo_" + std::to_string(msg->id_) + ".jpg";
                    info.file_size = largest->photo_->size_;
                    found_files_.push_back(info);

                    std::cout << "  " << index << ". " << info.file_name
                              << " (" << info.file_size << " bajtów)" << std::endl;
                }
            } else if (content_id == td::td_api::messageVideo::ID) {
                auto *video_msg = static_cast<td::td_api::messageVideo *>(msg->content_.get());
                if (video_msg->video_) {
                    index++;
                    FileInfo info;
                    info.message_id = msg->id_;
                    info.file_id = video_msg->video_->video_->id_;
                    info.file_name = video_msg->video_->file_name_;
                    info.file_size = video_msg->video_->video_->size_;
                    found_files_.push_back(info);

                    std::cout << "  " << index << ". " << info.file_name
                              << " (" << info.file_size << " bajtów)" << std::endl;
                }
            }
        }

        if (index == 0) {
            std::cout << "  (brak plików)" << std::endl;
        }

        show_menu();
    }

    void download_file(std::int32_t file_id) {
        auto request = td::td_api::make_object<td::td_api::downloadFile>();
        request->file_id_ = file_id;
        request->priority_ = 32;
        request->offset_ = 0;
        request->limit_ = 0;
        request->synchronous_ = false;

        is_downloading_ = true;
        downloading_file_id_ = file_id;

        std::cout << "[DEBUG] downloadFile: file_id=" << file_id
                  << ", priority=32, synchronous=false" << std::endl;
        std::cout << "[INFO] Pobieranie pliku (ID: " << file_id << ")..." << std::endl;
        client_manager_->send(client_id_, 9, std::move(request));
    }

    void handle_file_downloaded(td::td_api::object_ptr<td::td_api::file> file) {
        std::cout << "[DEBUG] handle_file_downloaded: file_id=" << file->id_
                  << ", size=" << file->size_
                  << ", expected_size=" << file->expected_size_ << std::endl;

        if (file->local_) {
            std::cout << "[DEBUG]   local: path=" << file->local_->path_
                      << ", is_downloading_active=" << file->local_->is_downloading_active_
                      << ", is_downloading_completed=" << file->local_->is_downloading_completed_
                      << ", downloaded_prefix_size=" << file->local_->downloaded_prefix_size_
                      << ", downloaded_size=" << file->local_->downloaded_size_ << std::endl;
        } else {
            std::cout << "[DEBUG]   local: nullptr!" << std::endl;
        }

        if (file->remote_) {
            std::cout << "[DEBUG]   remote: id=" << file->remote_->id_
                      << ", is_uploading_completed=" << file->remote_->is_uploading_completed_ << std::endl;
        }

        if (file->local_ && file->local_->is_downloading_completed_) {
            std::cout << "[Sukces] Plik pobrany z TDLib: " << file->local_->path_ << std::endl;
            move_to_downloads(file->local_->path_);
            is_downloading_ = false;
            downloading_file_id_ = 0;
            show_menu();
        } else if (file->local_ && file->local_->is_downloading_active_) {
            std::cout << "[DEBUG] Pobieranie aktywne, czekam na updateFile..." << std::endl;
        } else {
            std::cout << "[BŁĄD] Pobieranie nie rozpoczęte! Sprawdź file_id i stan pliku." << std::endl;
            is_downloading_ = false;
            downloading_file_id_ = 0;
            show_menu();
        }
    }
};

int main() {
    TelegramAuth app;
    app.run();
    return 0;
}
