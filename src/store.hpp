#pragma once
#include "catalog.hpp"
#include "crypto.hpp"
#include <deque>
#include <shared_mutex>

namespace tfs {
fs::path native_key_path(const Config&);
class Store {
public:
    Store(Config config, Telegram& telegram);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void start();
    void stop();
    void refresh_now();
    bool flush(bool force_remote);
    bool dirty() const;
    void wake();
    void request_sync();
    void open_file();
    void close_file();

    Entry find(std::wstring_view path) const;
    Entry current(const Entry& previous) const;
    Entry create(std::wstring_view path, uint32_t attributes, uint64_t allocation);
    std::vector<Entry> list(const Entry& directory, std::wstring_view after, size_t maximum) const;
    Entry parent(const Entry& node) const;
    void can_delete(const Entry&);
    void remove(const Entry&);
    Entry rename(const Entry&, std::wstring_view path, bool replace);
    void restore(std::wstring_view path);
    void mark(std::wstring_view path, bool favorite, bool value);
    std::vector<std::wstring> marked(bool favorite) const;
    Entry set_basic(const Entry&, uint32_t attr, uint64_t c, uint64_t a, uint64_t m, uint64_t w);
    Entry resize(const Entry&, uint64_t size, bool allocation_size, uint32_t overwrite_attributes = UINT32_MAX);

    size_t read(const Entry&, uint64_t offset, std::span<uint8_t> buffer);
    Entry write(const Entry&, uint64_t offset, std::span<const uint8_t> buffer,
        bool append, bool constrained, size_t& count);
    void prefetch(const Entry&, uint64_t offset, unsigned count);
    uint64_t free_space() const;
    const Config& config() const { return config_; }

private:
    using Children = std::map<std::wstring, std::string, OrdinalLess>;
    struct Commit {
        std::string id, parent;
        Doc doc;
    };
    struct Cached {
        std::shared_ptr<const Bytes> data;
        uint64_t disk_size = 0;
        uint64_t access = 0;
        bool pending = false;
    };
    Entry locate(std::wstring_view path) const;
    std::pair<Entry, std::wstring> split_path(std::wstring_view) const;
    Entry latest(const Entry& node) const;
    void writable(const Entry& node) const;
    void check_delete(const Entry&) const;
    void set_entry(Entry);
    void transaction(std::vector<Node>);
    void load();
    void load_imports();
    void rebuild_imports();
    void validate_catalog(const Entries& candidate) const;
    void checkpoint(const Entries&, uint64_t generation, const std::string& head);
    void restore_checkpoint();
    void discard_journal(uint64_t max_generation);
    Bytes wrap(std::span<const uint8_t>, std::string_view context) const;
    Bytes unwrap(std::span<const uint8_t>, std::string_view context) const;
    std::optional<Commit> remote_index();
    void sync();
    void run(std::stop_token);
    void run_prefetch(std::stop_token);
    void pull(const Commit&);
    void rebase(const Commit&);
    void replace_native(const Entries&);
    Doc upload_block(const std::string&);
    void publish(Entries entries, uint64_t generation, const std::string& parent);
    void finish_flight(const std::optional<Commit>&);
    Entries decode_index(const Json&, const Commit&) const;
    std::shared_ptr<const Bytes> block(const Node&, uint64_t index, int priority);
    std::shared_ptr<const Bytes> native_block(const std::string&, int priority);
    std::string stage_block(Bytes);
    void resize_node(Node&, uint64_t);
    void trim_cache(uint64_t incoming);
    void remember(const std::string&, std::shared_ptr<const Bytes>, uint64_t, bool);
    fs::path cache_file(const std::string&) const;
    fs::path tx_file(uint64_t) const;
    void gc_pending();

    Config config_;
    Telegram& tg_;
    fs::path dir_;
    Secret key_;
    mutable std::recursive_mutex mutex_;
    Entries nodes_;
    std::map<std::string, Children, std::less<>> children_;
    std::vector<std::weak_ptr<const Node>> retired_;
    std::mutex refs_mutex_;
    std::unordered_map<std::string, Doc> refs_;
    uint64_t generation_ = 0, committed_ = 0, last_rebase_ = 0;
    size_t open_files_ = 0;
    std::string head_;
    std::string previous_head_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> refresh_requested_{false};
    Clock::time_point due_;
    std::mutex sync_mutex_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::jthread worker_;
    std::vector<std::jthread> prefetchers_;
    std::deque<std::pair<Entry, uint64_t>> queue_;
    std::condition_variable prefetch_cv_;

    mutable std::mutex cache_mutex_;
    std::array<std::mutex, 127> block_mutexes_;
    std::unordered_map<std::string, Cached> cache_;
    uint64_t cache_tick_ = 0, memory_bytes_ = 0, disk_bytes_ = 0;
};
}
