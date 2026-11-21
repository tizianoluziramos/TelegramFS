#pragma once
#include "common.hpp"
#include <condition_variable>
#include <future>
#include <mutex>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tfs {
inline constexpr std::string_view index_tag = "#telegramfs-index/1:";
inline constexpr std::string_view block_tag = "#telegramfs-block/1:";
inline constexpr std::string_view key_tag = "#telegramfs-key/1";
inline constexpr std::string_view legacy_key_tag = "#driveinfinity-keybackup";
inline constexpr std::string_view legacy_folders_tag = "#driveinfinity-folders";

struct TdError : std::runtime_error {
    int code;
    TdError(int value, std::string message);
};
struct Doc {
    int64_t chat = 0;
    int64_t message = 0;
    uint64_t size = 0;
    int64_t modified_ms = 0;
    std::string name;
    std::string caption;
    std::string unique;
};
void to_json(Json&, const Doc&);
void from_json(const Json&, Doc&);

Json obj(std::string_view type);
std::string type_of(const Json&);
void save_api_credentials(const Config&, int id, std::string_view hash);

class Telegram {
public:
    explicit Telegram(Config);
    ~Telegram();
    Telegram(const Telegram&) = delete;
    Telegram& operator=(const Telegram&) = delete;
    void authenticate(bool interactive);
    void logout();
    Json request(Json query, std::chrono::seconds timeout = std::chrono::seconds(35));
    int64_t create_channel(std::string_view title);
    void verify_channel(int64_t chat, bool writing);
    std::vector<Json> channels();
    void search(int64_t chat, std::string_view query,
        const std::function<bool(const Doc&)>& visitor);
    Doc send_document(int64_t chat, const fs::path& path, std::string_view caption);
    Bytes download(const Doc&, uint64_t offset, size_t count, int priority = 16);
    void stop_io();
    bool connected() const noexcept { return connected_.load(); }
    uint64_t revision() const noexcept { return changes_.load(); }

private:
    Json await_auth();
    void receive_loop(std::stop_token);
    void accept(Json);
    void send(Json);
    void close_client() noexcept;
    void wait_rate();
    void note_rate(const std::string&);

    Config config_;
    int client_ = 0;
    std::jthread receiver_;
    std::atomic<bool> closing_{false}, stopped_{false}, connected_{false};
    std::atomic<uint64_t> seq_{0}, changes_{0};
    std::mutex mutex_;
    std::condition_variable condition_;
    Clock::time_point rate_until_{};
    Json auth_;
    std::unordered_map<uint64_t, std::promise<Json>> requests_;
    std::unordered_map<int64_t, Json> send_results_;
    std::unordered_set<int64_t> opened_chats_;
    std::counting_semaphore<16> downloads_;
    std::counting_semaphore<16> uploads_;
    std::array<std::mutex, 61> ranges_;
};
}
