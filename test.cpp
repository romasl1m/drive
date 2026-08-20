#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>
#include <vector>
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

        auto get_version = td::td_api::make_object<td::td_api::getOption>();
        get_version->name_ = "version";
        client_manager_->send(client_id_, 0, std::move(get_version));
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

    struct FileInfo {
        std::int64_t message_id;
        std::int32_t file_id;
        std::string file_name;
        std::int64_t file_size;
    };
    std::vector<FileInfo> found_files_;

    void process_response(std::int64_t request_id, td::td_api::object_ptr<td::td_api::Object> object) {
        if (not object) {
            return;
        }

        std::int32_t id = object->get_id();

        if (id == td::td_api::updateAuthorizationState::ID) {
            auto auth_state = td::move_tl_object_as<td::td_api::updateAuthorizationState>(object);
            handle_auth_state(std::move(auth_state->authorization_state_));
        } else if (id == td::td_api::chat::ID) {
            auto chat = td::move_tl_object_as<td::td_api::chat>(object);

            if (request_id == 4) {
                created_chat_id_ = chat->id_;
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
            if (update->file_ && update->file_->local_ && update->file_->local_->is_downloading_completed_) {
                std::cout << "[Sukces] Pobrano plik: " << update->file_->local_->path_ << std::endl;
                show_menu();
            }
        } else if (id == td::td_api::error::ID) {
            auto error = td::move_tl_object_as<td::td_api::error>(object);
            std::cerr << "\n[BŁĄD TELEGRAMA] Request ID: " << request_id
                      << " | Kod: " << error->code_
                      << " | Wiadomość: " << error->message_ << std::endl;
            if (is_authorized_ && created_chat_id_ != 0) {
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
                std::cout << "\n[STAN] Zalogowano pomyślnie! Tworzenie kanału..." << std::endl;
                create_private_channel();
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
        request->synchronous_ = true;

        std::cout << "[INFO] Pobieranie pliku (ID: " << file_id << ")..." << std::endl;
        client_manager_->send(client_id_, 9, std::move(request));
    }

    void handle_file_downloaded(td::td_api::object_ptr<td::td_api::file> file) {
        if (file->local_ && file->local_->is_downloading_completed_) {
            std::cout << "[Sukces] Plik pobrany do: " << file->local_->path_ << std::endl;
        } else {
            std::cout << "[INFO] Pobieranie w toku... oczekiwanie na updateFile." << std::endl;
        }
        show_menu();
    }
};

int main() {
    TelegramAuth app;
    app.run();
    return 0;
}
