#include "telegram.hpp"
#include "crypto.hpp"
#include <td/telegram/td_json_client.h>
#include <charconv>
#include <iostream>

namespace tfs {
using namespace std::chrono_literals;
namespace {
Doc document(const Json& message) {
    const auto& content = message.at("content");
    const auto& media = content.at("document");
    const auto& file = media.at("document");
    return {message.at("chat_id").get<int64_t>(), message.at("id").get<int64_t>(),
        file.at("size").get<uint64_t>(),
        1000 * message.value("date", int64_t{0}), media.value("file_name", std::string{}),
        content.at("caption").value("text", std::string{}),
        file.at("remote").value("unique_id", std::string{})};
}
}

TdError::TdError(int value, std::string message)
    : std::runtime_error("Telegram " + std::to_string(value) + ": " + std::move(message)), code(value) {}
Json obj(std::string_view type) { return {{"@type", type}}; }
std::string type_of(const Json& value) { return value.is_object() ? value.value("@type", std::string{}) : ""; }

void to_json(Json& j, const Doc& d) {
    j = {{"c",d.chat},{"m",d.message},{"s",d.size},{"t",d.modified_ms},{"n",d.name},
        {"p",d.caption},{"u",d.unique}};
}
void from_json(const Json& j, Doc& d) {
    j.at("c").get_to(d.chat);
    j.at("m").get_to(d.message);
    j.at("s").get_to(d.size);
    j.at("t").get_to(d.modified_ms);
    j.at("n").get_to(d.name);
    j.at("p").get_to(d.caption);
    j.at("u").get_to(d.unique);
    if (d.chat >= 0 || d.message <= 0) throw std::runtime_error("Invalid remote document reference");
}

void save_api_credentials(const Config& config, int id, std::string_view hash) {
    if (id <= 0 || hash.size() != 32 || !std::ranges::all_of(hash, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    })) throw std::runtime_error("Invalid Telegram API ID or API hash");
    std::string value = std::to_string(id) + ":" + std::string(hash);
    Finally wipe{[&] { SecureZeroMemory(value.data(), value.size()); }};
    save_secret(config.root / L"telegram-api.dpapi", bytes_of(value));
}

Telegram::Telegram(Config config)
    : config_(std::move(config)), downloads_(config_.downloads), uploads_(config_.uploads) {
    try {
        td_execute(R"({"@type":"setLogVerbosityLevel","new_verbosity_level":0})");
        client_ = td_create_client_id();
        receiver_ = std::jthread([this](std::stop_token token) { receive_loop(token); });
        send(obj("getAuthorizationState"));
    } catch (...) {
        close_client();
        throw;
    }
}

Telegram::~Telegram() {
    close_client();
}

void Telegram::close_client() noexcept {
    try {
        if (!receiver_.joinable()) return;
        std::unique_lock lock(mutex_);
        if (type_of(auth_) != "authorizationStateClosed") {
            send(obj("close"));
            condition_.wait_for(lock, 8s, [&] { return type_of(auth_) == "authorizationStateClosed"; });
        }
    } catch (...) {}
    receiver_.request_stop();
    receiver_.join();
}

void Telegram::stop_io() {
    stopped_ = true;
    condition_.notify_all();
}

void Telegram::send(Json query) {
    std::string encoded = query.dump();
    Finally wipe{[&] { SecureZeroMemory(encoded.data(), encoded.size()); }};
    td_send(client_, encoded.c_str());
}

void Telegram::receive_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            if (const char* raw = td_receive(0.25)) {
                auto value = Json::parse(raw);
                if (value.value("@client_id", client_) == client_) accept(std::move(value));
            }
        } catch (const std::exception& e) {
            log(config_, e.what());
        }
    }
}

void Telegram::note_rate(const std::string& message) {
    int seconds = 60;
    auto start = message.find_last_not_of("0123456789");
    if (start != std::string::npos && start + 1 < message.size()) {
        auto parsed = std::from_chars(message.data() + start + 1, message.data() + message.size(), seconds);
        if (parsed.ec != std::errc{}) seconds = 60;
    }
    rate_until_ = std::max(rate_until_, Clock::now() + std::chrono::seconds(std::clamp(seconds, 1, 86400)));
}

void Telegram::accept(Json value) {
    std::lock_guard lock(mutex_);
    const auto type = type_of(value);
    if (type == "error" && value.value("code", 0) == 429) note_rate(value.value("message", std::string{}));
    if (type == "updateAuthorizationState") {
        auth_ = value.at("authorization_state");
        closing_ = type_of(auth_) == "authorizationStateClosed";
    } else if (type == "updateConnectionState") {
        connected_ = type_of(value.at("state")) == "connectionStateReady";
    } else if (type == "updateMessageSendSucceeded" || type == "updateMessageSendFailed") {
        const auto id = value.at("old_message_id").get<int64_t>();
        if (type == "updateMessageSendFailed" && value.at("error").value("code", 0) == 429)
            note_rate(value.at("error").value("message", std::string{}));
        if (send_results_.size() >= 128) send_results_.erase(send_results_.begin());
        send_results_[id] = std::move(value);
        ++changes_;
    } else if (type == "updateNewMessage" || type == "updateMessageContent" || type == "updateMessageEdited" ||
        (type == "updateDeleteMessages" && value.value("is_permanent", false) && !value.value("from_cache", true))) {
        ++changes_;
    }
    if (value.contains("@extra") && value["@extra"].is_number_unsigned()) {
        const auto id = value["@extra"].get<uint64_t>();
        if (auto it = requests_.find(id); it != requests_.end()) {
            it->second.set_value(std::move(value));
            requests_.erase(it);
        }
    }
    condition_.notify_all();
}

void Telegram::wait_rate() {
    std::lock_guard lock(mutex_);
    if (stopped_ || closing_) throw TdError(499, "Session is closing");
    if (Clock::now() < rate_until_) throw TdError(429, "Rate limit is still active");
}

Json Telegram::request(Json query, std::chrono::seconds timeout) {
    wait_rate();
    const auto id = ++seq_;
    query["@extra"] = id;
    std::future<Json> result;
    {
        std::lock_guard lock(mutex_);
        result = requests_.try_emplace(id).first->second.get_future();
    }
    Finally remove{[&] {
        std::lock_guard lock(mutex_);
        requests_.erase(id);
    }};
    send(std::move(query));
    auto deadline = Clock::now() + timeout;
    while (result.wait_for(200ms) != std::future_status::ready) {
        if (stopped_ || closing_) throw TdError(499, "Canceled");
        if (Clock::now() >= deadline) throw TdError(408, "Request timed out");
    }
    auto value = result.get();
    if (type_of(value) == "error")
        throw TdError(value.value("code", 500), value.value("message", std::string{}));
    return value;
}

Json Telegram::await_auth() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, 35s, [&] { return !auth_.empty() || stopped_; }))
        throw TdError(408, "Authorization state unavailable");
    return auth_;
}

void Telegram::authenticate(bool interactive) {
    const auto credentials_path = config_.root / L"telegram-api.dpapi";
    if (!fs::exists(credentials_path)) {
        if (!interactive) throw std::runtime_error("Run telegramfs login first");
        const auto id_text = utf8(read_line(L"API ID: "));
        int id{};
        const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size())
            throw std::runtime_error("Invalid API ID");
        auto hash = utf8(read_password(L"API hash: "));
        Finally wipe{[&] { SecureZeroMemory(hash.data(), hash.size()); }};
        save_api_credentials(config_, id, hash);
    }
    auto version = obj("getOption");
    version["name"] = "version";
    if (request(std::move(version)).value("value", std::string{}) != "1.8.70")
        throw std::runtime_error("TDLib 1.8.70 is required for this Telegram API schema");
    auto state = await_auth();
    for (;;) {
        const auto type = type_of(state);
        if (type == "authorizationStateReady") return;
        Json query;
        if (type == "authorizationStateWaitTdlibParameters") {
            const auto db = config_.root / L"tdlib";
            const auto files = db / L"files";
            fs::create_directories(files);
            const auto key_file = config_.root / L"telegram-db.dpapi";
            if (!fs::exists(key_file)) {
                const Secret key(random_bytes(32));
                save_secret(key_file, key.bytes);
            }
            const auto credentials = load_secret(credentials_path);
            const auto text = std::string_view(reinterpret_cast<const char*>(credentials.bytes.data()),
                credentials.bytes.size());
            const auto colon = text.find(':');
            if (colon == std::string::npos) throw std::runtime_error("Invalid saved API credentials");
            query = obj("setTdlibParameters");
            query.update({{"database_directory", utf8(db.wstring())}, {"files_directory", utf8(files.wstring())},
                {"database_encryption_key", encode_base64(load_secret(key_file).bytes)}, {"use_file_database", true},
                {"use_chat_info_database", true}, {"use_message_database", true}, {"use_secret_chats", false},
                {"api_id", std::stoi(std::string(text.substr(0, colon)))}, {"api_hash", text.substr(colon + 1)},
                {"system_language_code", "en"}, {"device_model", "TelegramFS"},
                {"system_version", "Windows"}, {"application_version", "0.1.0"}});
        } else if (!interactive) {
            throw std::runtime_error("Telegram session needs attention. Run telegramfs login");
        } else if (type == "authorizationStateWaitPhoneNumber") {
            query = obj("setAuthenticationPhoneNumber");
            query["phone_number"] = utf8(read_line(L"Phone number (+country code): "));
        } else if (type == "authorizationStateWaitCode") {
            query = obj("checkAuthenticationCode");
            query["code"] = utf8(read_password(L"Telegram code: "));
        } else if (type == "authorizationStateWaitPassword") {
            query = obj("checkAuthenticationPassword");
            query["password"] = utf8(read_password(L"Telegram two-step password: "));
        } else if (type == "authorizationStateWaitEmailAddress") {
            query = obj("setAuthenticationEmailAddress");
            query["email_address"] = utf8(read_line(L"Telegram email address: "));
        } else if (type == "authorizationStateWaitEmailCode") {
            query = obj("checkAuthenticationEmailCode");
            query["code"] = {{"@type", "emailAddressAuthenticationCode"},
                {"code", utf8(read_password(L"Telegram email code: "))}};
        } else {
            throw std::runtime_error("Unsupported authorization state: " + type);
        }
        try {
            request(std::move(query), 60s);
        } catch (const TdError& e) {
            if (!interactive || type == "authorizationStateWaitTdlibParameters" || e.code != 400) throw;
            std::cerr << e.what() << '\n';
            continue;
        }
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 60s, [&] { return auth_ != state || stopped_; }))
            throw TdError(408, "Authorization update timed out");
        state = auth_;
    }
}

void Telegram::logout() {
    request(obj("logOut"));
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, 35s, [&] { return type_of(auth_) == "authorizationStateClosed"; }))
        throw TdError(408, "Logout did not finish");
}

int64_t Telegram::create_channel(std::string_view title) {
    auto q = obj("createNewSupergroupChat");
    q.update({{"title", title}, {"is_channel", true}, {"description", "TelegramFS private storage"}});
    return request(std::move(q)).at("id").get<int64_t>();
}

void Telegram::verify_channel(int64_t chat, bool writing) {
    if (chat >= 0) throw std::runtime_error("A private Telegram channel ID is required");
    auto q = obj("getChat");
    q["chat_id"] = chat;
    Json result;
    try {
        result = request(q);
    } catch (const TdError& error) {
        if (error.code != 400 && error.code != 404) throw;
        (void)channels();
        result = request(q);
    }
    const auto& type = result.at("type");
    if (type_of(type) != "chatTypeSupergroup" || !type.value("is_channel", false))
        throw std::runtime_error("The configured chat is not a channel");
    q = obj("getSupergroup");
    q["supergroup_id"] = type.at("supergroup_id");
    const auto group = request(std::move(q));
    const auto usernames = group.value("usernames", Json{});
    if (usernames.is_object() && !usernames.value("active_usernames", Json::array()).empty())
        throw std::runtime_error("The configured channel has a public username");
    if (writing) {
        const auto& member = group.at("status");
        if (type_of(member) != "chatMemberStatusCreator" &&
            (type_of(member) != "chatMemberStatusAdministrator" ||
                !member.at("rights").value("can_post_messages", false)))
            throw std::runtime_error("Telegram account cannot post to the designated channel");
        if (result.value("message_auto_delete_time", 0))
            throw std::runtime_error("Disable automatic message deletion on the write channel");
    }
}

std::vector<Json> Telegram::channels() {
    for (unsigned i = 0; i < 100; ++i) {
        auto q = obj("loadChats");
        q["limit"] = 100;
        try { request(std::move(q)); }
        catch (const TdError& error) { if (error.code == 404) break; throw; }
    }
    auto q = obj("getChats");
    q["limit"] = 10000;
    std::vector<Json> result;
    for (const auto& id : request(std::move(q)).at("chat_ids").get<std::vector<int64_t>>()) {
        auto get = obj("getChat");
        get["chat_id"] = id;
        auto chat = request(std::move(get));
        if (type_of(chat.at("type")) == "chatTypeSupergroup" && chat.at("type").value("is_channel", false))
            result.push_back(std::move(chat));
    }
    return result;
}

void Telegram::search(int64_t chat, std::string_view query,
    const std::function<bool(const Doc&)>& visitor) {
    bool needs_open;
    {
        std::lock_guard lock(mutex_);
        needs_open = !opened_chats_.contains(chat);
    }
    if (needs_open) {
        auto open = obj("openChat");
        open["chat_id"] = chat;
        request(std::move(open));
        std::lock_guard lock(mutex_);
        opened_chats_.insert(chat);
    }
    // Walk the chat history directly when there is no text to search for.
    const bool history = query.empty();
    int64_t from = 0;
    for (;;) {
        auto q = obj(history ? "getChatHistory" : "searchChatMessages");
        q.update({{"chat_id", chat}, {"from_message_id", from}, {"limit", 100}});
        if (history) q["only_local"] = false;
        else q["query"] = query;
        const auto page = request(std::move(q));
        auto next = page.value("next_from_message_id", int64_t{0});
        for (const auto& message : page.at("messages")) {
            if (history) {
                const auto id = message.at("id").get<int64_t>();
                if (from && id >= from) continue;
                next = id;
            }
            if (type_of(message.at("content")) == "messageDocument" &&
                message.value("sending_state", Json{}).is_null() && !visitor(document(message))) return;
        }
        if (!next) break;
        if (from && next >= from) throw std::runtime_error("Telegram returned a non-advancing search cursor");
        from = next;
    }
}

Doc Telegram::send_document(int64_t chat, const fs::path& path, std::string_view caption) {
    while (!uploads_.try_acquire_for(200ms)) if (stopped_) throw TdError(499, "Upload canceled");
    Finally slot{[&] { uploads_.release(); }};
    auto q = obj("sendMessage");
    Json file = {{"@type", "inputFileLocal"}, {"path", utf8(path.wstring())}};
    Json doc = {{"@type", "inputDocument"}, {"document", file}, {"disable_content_type_detection", true}};
    q["chat_id"] = chat;
    q["options"] = {{"@type", "messageSendOptions"}, {"disable_notification", true}, {"from_background", true}};
    q["input_message_content"] = {{"@type", "inputMessageDocument"}, {"document", doc},
        {"caption", {{"@type", "formattedText"}, {"text", caption}, {"entities", Json::array()}}}};
    const auto message = request(std::move(q));
    if (type_of(message.value("sending_state", Json{})).empty()) return document(message);
    const auto id = message.at("id").get<int64_t>();
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, 3min, [&] { return stopped_ || closing_ || send_results_.contains(id); }))
        throw TdError(408, "Upload confirmation did not arrive");
    if (stopped_ || closing_) throw TdError(499, "Upload canceled");
    auto result = send_results_.extract(id).mapped();
    if (type_of(result) == "updateMessageSendFailed") {
        const auto& e = result.at("error");
        throw TdError(e.value("code", 500), e.value("message", std::string{}));
    }
    return document(result.at("message"));
}

Bytes Telegram::download(const Doc& doc, uint64_t offset, size_t count, int priority) {
    if (offset > doc.size || count > doc.size - offset)
        throw WinError(ERROR_INVALID_DATA, "Invalid remote byte range");
    if (!count) return {};
    while (!downloads_.try_acquire_for(200ms)) if (stopped_) throw TdError(499, "Download canceled");
    Finally slot{[&] { downloads_.release(); }};
    auto get = obj("getMessage");
    get.update({{"chat_id", doc.chat}, {"message_id", doc.message}});
    const auto message = request(std::move(get));
    if (type_of(message.at("content")) != "messageDocument")
        throw WinError(ERROR_INVALID_DATA, "Remote object is not a document");
    const auto& expected = message.at("content").at("document").at("document");
    if ((!doc.unique.empty() && expected.at("remote").value("unique_id", std::string{}) != doc.unique) ||
        expected.value("size", uint64_t{0}) != doc.size)
        throw WinError(ERROR_INVALID_DATA, "Remote document content changed");
    const int file_id = expected.at("id").get<int>();
    std::lock_guard range(ranges_[static_cast<size_t>(file_id) % ranges_.size()]);
    auto q = obj("downloadFile");
    q.update({{"file_id", file_id}, {"priority", priority}, {"offset", offset},
        {"limit", count}, {"synchronous", true}});
    Json result;
    try {
        result = request(std::move(q), 45s);
    } catch (const TdError& e) {
        if (e.code == 408 || e.code == 499) {
            auto cancel = obj("cancelDownloadFile");
            cancel["file_id"] = file_id;
            cancel["only_if_pending"] = false;
            send(std::move(cancel));
        }
        throw;
    }
    const auto& local = result.at("local");
    const auto base = local.value("download_offset", uint64_t{0});
    const auto prefix = local.value("downloaded_prefix_size", uint64_t{0});
    if (local.value("path", std::string{}).empty() || (!local.value("is_downloading_completed", false) &&
        (base > offset || offset - base > prefix || count > prefix - (offset - base))))
        throw TdError(408, "Requested download range is incomplete");
    auto bytes = read_range(wide(local.at("path").get<std::string>()), offset, count);
    if (local.value("can_be_deleted", false)) {
        try {
            auto clean = obj("deleteFile");
            clean["file_id"] = file_id;
            request(std::move(clean), 4s);
        } catch (const TdError& e) { log(config_, e.what(), false); }
    }
    return bytes;
}
}
