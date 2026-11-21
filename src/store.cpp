#include "store.hpp"
#include <charconv>
#include <set>

namespace tfs {
using namespace std::chrono_literals;
namespace {
constexpr uint32_t attributes_mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
    FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
constexpr int normal_priority = 24;
constexpr uint64_t handle_reserve = 8192;
constexpr auto index_name = L"telegramfsindex.bin";
constexpr auto block_name = L"telegramfsblock.bin";

uint64_t round_allocation(uint64_t size) { return (size + 4095) & ~uint64_t(4095); }

uint32_t normalized_attributes(uint32_t value, bool directory) {
    const auto normalized = (value & attributes_mask) | (directory ? FILE_ATTRIBUTE_DIRECTORY : 0);
    return normalized ? normalized : FILE_ATTRIBUTE_NORMAL;
}

bool system_node(std::string_view id) {
    return id == root_id || id == trash_id || id == imports_id;
}

bool object_id(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t pos = 0; pos < id.size(); ++pos) {
        char ch = id[pos];
        if (pos == 8 || pos == 13 || pos == 18 || pos == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F'))) return false;
    }
    return true;
}

std::optional<uint64_t> sequence(const fs::path& path) {
    if (path.extension() != L".j") return {};
    const auto stem = path.stem().string();
    uint64_t value = 0;
    const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size()) return {};
    return value;
}

std::string local_id(const Node& n, uint64_t block) {
    auto version = n.id + "/" + std::to_string(n.modified) + "/" + std::to_string(n.size);
    for (const auto& part : n.parts) version += "/" + part.doc.unique + "/" + std::to_string(part.doc.message);
    return "L" + digest(bytes_of(version + "/" + std::to_string(block)));
}

Json native_nodes(const Entries& entries) {
    auto array = Json::array();
    for (const auto& [id, entry] : entries) {
        if (!system_node(id) && !entry->legacy_chat) array.push_back(*entry);
    }
    return array;
}

fs::path key_path(const Config& config) {
    return config.root / (L"key-" + std::to_wstring(config.channel) + L".dpapi");
}

struct StagingFile {
    fs::path directory;
    fs::path path;
    StagingFile(const fs::path& root, std::wstring_view name)
        : directory(root / wide(uuid())), path(directory / name) {
        fs::create_directory(directory);
    }
    ~StagingFile() {
        erase_if_exists(path);
        erase_if_exists(directory);
    }
};
}

fs::path native_key_path(const Config& config) { return key_path(config); }

void to_json(Json& j, const Part& p) { j = Json::array({p.offset, p.length, p.doc}); }
void from_json(const Json& j, Part& p) {
    j.at(0).get_to(p.offset);
    j.at(1).get_to(p.length);
    j.at(2).get_to(p.doc);
}
void to_json(Json& j, const Node& n) {
    j = {{"i",n.id},{"p",n.parent},{"n",utf8(n.name)},{"s",n.size},{"l",n.allocation},
        {"ct",n.creation},{"at",n.access},{"mt",n.modified},{"wt",n.change},{"a",n.attributes}};
    if (!n.original_parent.empty()) j["restore"] = {n.original_parent, utf8(n.original_name)};
    if (!n.blocks.empty()) {
        auto blocks = Json::array();
        for (const auto& [i, key] : n.blocks) blocks.push_back(Json::array({i, key}));
        j["b"] = std::move(blocks);
    }
    if (n.legacy_chat) {
        j["chat"] = n.legacy_chat;
        j["e"] = n.encrypted;
        j["parts"] = n.parts;
        j["broken"] = n.incomplete;
    }
    if (n.favorite) j["favorite"] = true;
    if (n.archived) j["archived"] = true;
}
void from_json(const Json& j, Node& n) {
    j.at("i").get_to(n.id);
    j.at("p").get_to(n.parent);
    n.name = wide(j.at("n").get<std::string>());
    validate_name(n.name);
    j.at("s").get_to(n.size);
    j.at("l").get_to(n.allocation);
    j.at("ct").get_to(n.creation);
    j.at("at").get_to(n.access);
    j.at("mt").get_to(n.modified);
    j.at("wt").get_to(n.change);
    j.at("a").get_to(n.attributes);
    if (n.size > file_limit || n.allocation > file_limit || n.id.empty() || n.parent.empty() ||
        n.id.size() > 64 || n.parent.size() > 64 || (n.directory() && n.size) ||
        n.attributes != normalized_attributes(n.attributes, n.directory()))
        throw WinError(ERROR_FILE_CORRUPT, "Invalid file index entry");
    if (auto it = j.find("restore"); it != j.end()) {
        n.original_parent = it->at(0).get<std::string>();
        n.original_name = wide(it->at(1).get<std::string>());
        validate_name(n.original_name);
    }
    if (auto it = j.find("b"); it != j.end()) {
        for (const auto& block : *it) {
            const auto index = block.at(0).get<uint64_t>();
            auto key = block.at(1).get<std::string>();
            if (index >= (n.size + block_size - 1) / block_size || !is_digest(key) ||
                !n.blocks.emplace(index, std::move(key)).second)
                throw WinError(ERROR_FILE_CORRUPT, "Invalid content block allocation");
        }
    }
    n.legacy_chat = j.value("chat", int64_t{0});
    n.encrypted = j.value("e", false);
    n.incomplete = j.value("broken", false);
    n.parts = j.value("parts", std::vector<Part>{});
    n.favorite = j.value("favorite", false);
    n.archived = j.value("archived", false);
}

Node directory(std::string id, std::string parent, std::wstring name, uint32_t attributes) {
    Node n;
    n.id = std::move(id);
    n.parent = std::move(parent);
    n.name = std::move(name);
    n.creation = n.access = n.modified = n.change = filetime();
    n.attributes = normalized_attributes(attributes, true);
    return n;
}

std::string stable_id(std::string_view text) { return digest(bytes_of(text)); }

Store::Store(Config config, Telegram& telegram)
    : config_(std::move(config)), tg_(telegram),
      dir_(config_.root / (L"channel-" + std::to_wstring(config_.channel))) {
    if (config_.encryption) {
        if (!fs::exists(key_path(config_))) throw std::runtime_error("Run key-init or key-restore for this channel");
        auto secret = load_secret(key_path(config_));
        if (secret.bytes.size() != 32) throw std::runtime_error("Invalid local channel encryption key");
        key_.bytes = std::move(secret.bytes);
    }
    fs::create_directories(dir_ / L"journal");
    fs::create_directories(dir_ / L"blocks");
    fs::create_directories(dir_ / L"receipts");
    fs::create_directories(dir_ / L"stage");
    for (const auto& file : fs::recursive_directory_iterator(dir_ / L"stage")) {
        if (file.is_regular_file()) erase_if_exists(file.path());
    }
    for (const auto& path : {dir_, dir_ / L"journal", dir_ / L"blocks", dir_ / L"receipts"}) {
        for (const auto& item : fs::directory_iterator(path))
            if (item.is_regular_file() && item.path().extension() == L".tmp") erase_if_exists(item.path());
    }
    load();
}
Store::~Store() { stop(); }

Bytes Store::wrap(std::span<const uint8_t> data, std::string_view context) const {
    const auto magic = bytes_of(key_.bytes.empty() ? "TFSP1" : "TFSE1");
    Bytes result(magic.begin(), magic.end());
    if (key_.bytes.empty()) result.insert(result.end(), data.begin(), data.end());
    else {
        const auto encrypted = seal(key_.bytes, data, context);
        result.insert(result.end(), encrypted.begin(), encrypted.end());
    }
    return result;
}

Bytes Store::unwrap(std::span<const uint8_t> data, std::string_view context) const {
    if (data.size() < 5) throw WinError(ERROR_FILE_CORRUPT, "Invalid storage envelope");
    const auto magic = string_of(data.first(5));
    if (magic == "TFSP1" && key_.bytes.empty()) return Bytes(data.begin() + 5, data.end());
    if (magic == "TFSE1" && !key_.bytes.empty()) return unseal(key_.bytes, data.subspan(5), context);
    throw WinError(ERROR_INVALID_DATA, "Channel data and encryption settings do not match");
}

fs::path Store::tx_file(uint64_t sequence) const {
    return dir_ / L"journal" / (std::to_wstring(sequence) + L".j");
}
fs::path Store::cache_file(const std::string& key) const { return dir_ / L"blocks" / wide(key); }

void Store::load() {
    set_entry(std::make_shared<Node>(directory(std::string(root_id), "", L"", 0)));
    set_entry(std::make_shared<Node>(directory(std::string(trash_id), std::string(root_id), L".Papelera",
        FILE_ATTRIBUTE_HIDDEN)));
    set_entry(std::make_shared<Node>(directory(std::string(imports_id), std::string(root_id), L".DriveInfinity",
        FILE_ATTRIBUTE_READONLY)));
    restore_checkpoint();
    for (const auto& item : fs::directory_iterator(dir_ / L"receipts")) {
        if (!item.is_regular_file()) continue;
        const auto key = item.path().filename().string();
        if (!is_digest(key)) continue;
        auto receipt = Json::parse(unwrap(read_file(item.path(), 65536), "receipt:" + key)).get<Doc>();
        if (receipt.chat != config_.channel || receipt.caption != std::string(block_tag) + key)
            throw WinError(ERROR_FILE_CORRUPT, "Invalid block receipt");
        refs_[key] = std::move(receipt);
    }

    std::map<uint64_t, fs::path> pending;
    for (const auto& item : fs::directory_iterator(dir_ / L"journal")) {
        if (const auto seq = sequence(item.path()); seq && *seq > committed_) pending.emplace(*seq, item.path());
    }
    for (const auto& [seq, path] : pending) {
        if (seq != generation_ + 1) throw WinError(ERROR_FILE_CORRUPT, "Local journal is incomplete");
        const auto data = unwrap(read_file(path, metadata_limit), "journal:" + std::to_string(seq));
        const auto journal = Json::parse(data);
        if (journal.is_object()) {
            auto id = journal.at("head").get<std::string>();
            auto parent = journal.at("parent").get<std::string>();
            auto candidate = decode_index(journal, Commit{id, parent, {}});
            const auto received = journal.at("refs").get<std::unordered_map<std::string, Doc>>();
            for (const auto& [key, doc] : received) refs_.try_emplace(key, doc);
            replace_native(candidate);
            previous_head_ = parent;
            head_ = id;
            last_rebase_ = seq;
        } else {
            for (auto& n : journal.get<std::vector<Node>>()) {
                if (!object_id(n.id) || n.legacy_chat) throw WinError(ERROR_FILE_CORRUPT, "Invalid journal node");
                set_entry(std::make_shared<Node>(std::move(n)));
            }
        }
        generation_ = seq;
    }
    validate_catalog(nodes_);
    discard_journal(committed_);
    for (const auto& item : fs::directory_iterator(dir_ / L"blocks")) {
        if (!item.is_regular_file()) continue;
        const auto name = item.path().filename().string();
        if (!is_digest(name) && !(name.starts_with("L") && is_digest(std::string_view(name).substr(1)))) continue;
        const auto size = item.file_size();
        disk_bytes_ += size;
        cache_[name] = {nullptr, size, 0, is_digest(name) && !refs_.contains(name)};
    }
    for (const auto& [id, n] : nodes_) {
        (void)id;
        for (const auto& [index, key] : n->blocks) {
            (void)index;
            if (!refs_.contains(key) && !cache_.contains(key))
                throw WinError(ERROR_FILE_CORRUPT, "A pending local block is missing");
        }
    }
    gc_pending();
    trim_cache(0);
    load_imports();
    due_ = Clock::now();
}

void Store::validate_catalog(const Entries& nodes) const {
    std::map<std::string, Children, std::less<>> children;
    for (const auto& [id, entry] : nodes) {
        if (id == root_id) continue;
        const auto parent = nodes.find(entry->parent);
        if (parent == nodes.end() || !parent->second->directory() ||
            (!entry->legacy_chat && entry->parent == imports_id) ||
            !children[entry->parent].emplace(entry->name, id).second)
            throw WinError(ERROR_FILE_CORRUPT, "Catalog parent or path collision");
        auto ancestor = parent;
        unsigned depth = 0;
        while (ancestor->first != root_id) {
            if (ancestor->first == id || ++depth > 1024)
                throw WinError(ERROR_FILE_CORRUPT, "Directory cycle or excessive depth");
            ancestor = nodes.find(ancestor->second->parent);
            if (ancestor == nodes.end()) throw WinError(ERROR_FILE_CORRUPT, "Missing ancestor");
        }
    }
}

void Store::restore_checkpoint() {
    const auto path = dir_ / L"checkpoint";
    if (!fs::exists(path)) return;
    const auto data = unwrap(read_file(path, metadata_limit), "checkpoint");
    const auto value = Json::parse(data);
    if (value.at("v") != 1) throw std::runtime_error("Unknown checkpoint format");
    value.at("g").get_to(generation_);
    value.at("head").get_to(head_);
    if (!head_.empty() && !object_id(head_)) throw std::runtime_error("Invalid checkpoint head");
    committed_ = generation_;
    for (auto& n : value.at("nodes").get<std::vector<Node>>()) {
        if (!object_id(n.id) || n.legacy_chat || nodes_.contains(n.id)) throw std::runtime_error("Bad checkpoint node");
        set_entry(std::make_shared<Node>(std::move(n)));
    }
    value.at("refs").get_to(refs_);
    for (const auto& [id, doc] : refs_)
        if (!is_digest(id) || doc.chat != config_.channel || doc.caption != std::string(block_tag) + id)
            throw std::runtime_error("Bad checkpoint reference");
    initialized_ = true;
}

void Store::checkpoint(const Entries& nodes, uint64_t generation, const std::string& head) {
    std::unordered_map<std::string, Doc> refs;
    {
        std::lock_guard lock(refs_mutex_);
        refs = refs_;
    }
    const auto data = Json{{"v",1},{"g",generation},{"head",head},{"nodes",native_nodes(nodes)},
        {"refs",std::move(refs)}}.dump();
    if (data.size() + 64 > metadata_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Local checkpoint exceeds 64 MiB");
    atomic_write(dir_ / L"checkpoint", wrap(bytes_of(data), "checkpoint"));
}

void Store::discard_journal(uint64_t max_generation) {
    std::error_code error;
    for (const auto& file : fs::directory_iterator(dir_ / L"journal", error)) {
        if (auto seq = sequence(file.path()); seq && *seq <= max_generation) erase_if_exists(file.path());
    }
}

void Store::load_imports() {
    const auto path = dir_ / L"imports";
    if (!fs::exists(path)) return;
    const auto data = dpapi(read_file(path, metadata_limit), true);
    auto nodes = Json::parse(data).get<std::vector<Node>>();
    Entries candidate = nodes_;
    for (auto& n : nodes) {
        if (n.legacy_chat != config_.channel && std::ranges::find(config_.imports, n.legacy_chat) == config_.imports.end())
            continue;
        if (!is_digest(n.id) || candidate.contains(n.id)) throw std::runtime_error("Invalid imported catalog");
        auto entry = std::make_shared<Node>(std::move(n));
        candidate.emplace(entry->id, entry);
    }
    validate_catalog(candidate);
    for (const auto& [id, node] : candidate) if (node->legacy_chat) set_entry(node);
}

void Store::rebuild_imports() {
    auto channels = config_.imports;
    if (channels.empty()) return;
    std::vector<Node> merged;
    for (auto channel : channels) {
        auto batch = read_legacy(config_, tg_, channel, true);
        for (auto& n : batch) merged.push_back(std::move(n));
    }
    auto raw = Json(merged).dump();
    if (raw.size() + 4096 > metadata_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Imported catalog exceeds 64 MiB");
    std::lock_guard lock(mutex_);
    if (open_files_) return;
    Entries candidate;
    for (const auto& [id, n] : nodes_) if (!n->legacy_chat) candidate.emplace(id, n);
    for (auto& n : merged) {
        auto entry = std::make_shared<Node>(std::move(n));
        if (!candidate.emplace(entry->id, entry).second) throw WinError(ERROR_FILE_CORRUPT, "Imported ID collision");
    }
    validate_catalog(candidate);
    atomic_write(dir_ / L"imports", dpapi(bytes_of(raw), false));
    nodes_.clear();
    children_.clear();
    for (const auto& [id, entry] : candidate) {
        (void)id;
        set_entry(entry);
    }
}

void Store::set_entry(Entry n) {
    if (auto found = nodes_.find(n->id); found != nodes_.end()) {
        const auto& old = found->second;
        if (old->parent != n->parent || old->name != n->name) children_[old->parent].erase(old->name);
        if (!old->blocks.empty() && old.use_count() > 1) retired_.push_back(old);
        found->second = n;
    } else {
        nodes_.emplace(n->id, n);
    }
    if (n->id != root_id) children_[n->parent][n->name] = n->id;
}

void Store::transaction(std::vector<Node> nodes) {
    const auto next = generation_ + 1;
    const auto data = Json(nodes).dump();
    if (data.size() + 64 > metadata_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Local transaction exceeds 64 MiB");
    if (free_space() < data.size() + handle_reserve) throw WinError(ERROR_DISK_FULL, "Insufficient journal space");
    std::vector<Entry> entries;
    entries.reserve(nodes.size());
    for (auto& node : nodes) entries.push_back(std::make_shared<Node>(std::move(node)));
    atomic_write(tx_file(next), wrap(bytes_of(data), "journal:" + std::to_string(next)));
    for (const auto& entry : entries) set_entry(entry);
    generation_ = next;
    due_ = config_.write_back ? Clock::now() + 1200ms : Clock::now();
    wake();
}

Entry Store::locate(std::wstring_view path) const {
    if (path.empty() || path.front() != L'\\' || path.size() > 32767)
        throw WinError(ERROR_INVALID_NAME, "Invalid absolute Windows path");
    auto node = nodes_.at(std::string(root_id));
    path.remove_prefix(1);
    unsigned depth = 0;
    while (!path.empty()) {
        if (++depth > 1024) throw WinError(ERROR_FILENAME_EXCED_RANGE, "Path exceeds the supported directory depth");
        const auto slash = path.find(L'\\');
        const auto part = path.substr(0, slash);
        validate_name(part);
        if (!node->directory()) throw WinError(ERROR_PATH_NOT_FOUND, "Ancestor is a file");
        auto children = children_.find(node->id);
        if (children != children_.end()) {
            auto match = children->second.find(part);
            if (match != children->second.end()) {
                node = nodes_.at(match->second);
                if (slash == std::wstring_view::npos) break;
                path.remove_prefix(slash + 1);
                continue;
            }
        }
        throw WinError(slash == std::wstring_view::npos ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND,
            "Path does not exist");
    }
    return node;
}

std::pair<Entry, std::wstring> Store::split_path(std::wstring_view path) const {
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring_view::npos || path.size() > 32767)
        throw WinError(ERROR_INVALID_NAME, "Invalid destination path");
    std::wstring name(path.substr(slash + 1));
    validate_name(name);
    auto parent = locate(path.substr(0, std::max<size_t>(1, slash)));
    if (!parent->directory()) throw WinError(ERROR_PATH_NOT_FOUND, "Parent is not a directory");
    return {std::move(parent), std::move(name)};
}

Entry Store::latest(const Entry& n) const {
    if (!n) throw WinError(ERROR_INVALID_HANDLE, "Missing file context");
    if (auto it = nodes_.find(n->id); it != nodes_.end()) return it->second;
    return n;
}
Entry Store::find(std::wstring_view path) const { std::lock_guard lock(mutex_); return locate(path); }
Entry Store::current(const Entry& node) const { std::lock_guard lock(mutex_); return latest(node); }

void Store::writable(const Entry& node) const {
    if (config_.read_only || node->legacy_chat || node->id == imports_id)
        throw WinError(ERROR_WRITE_PROTECT, "Read-only path");
    if (!nodes_.contains(node->id)) throw WinError(ERROR_FILE_NOT_FOUND, "File is no longer indexed");
}
Entry Store::create(std::wstring_view path, uint32_t attrs, uint64_t allocation) {
    std::lock_guard lock(mutex_);
    auto [parent, name] = split_path(path);
    writable(parent);
    if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ENCRYPTED))
        throw WinError(ERROR_NOT_SUPPORTED, "Unsupported requested file attributes");
    if (children_[parent->id].contains(name)) throw WinError(ERROR_ALREADY_EXISTS, "Path already exists");
    if (allocation > file_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Allocation exceeds Windows file limit");
    auto n = directory(uuid(), parent->id, name, 0);
    n.attributes = normalized_attributes(attrs, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if (!n.directory()) n.allocation = round_allocation(allocation);
    const auto id = n.id;
    transaction({std::move(n)});
    return nodes_.at(id);
}
std::vector<Entry> Store::list(const Entry& n, std::wstring_view after, size_t maximum) const {
    std::lock_guard lock(mutex_);
    std::vector<Entry> result;
    if (auto found = children_.find(n->id); found != children_.end()) {
        const auto& children = found->second;
        result.reserve(std::min(children.size(), maximum));
        auto it = after.empty() ? children.begin() : children.upper_bound(after);
        while (it != children.end() && result.size() < maximum) {
            result.push_back(nodes_.at(it->second));
            ++it;
        }
    }
    return result;
}
Entry Store::parent(const Entry& n) const {
    std::lock_guard lock(mutex_);
    if (auto it = nodes_.find(n->parent); it != nodes_.end()) return it->second;
    return nodes_.at(std::string(root_id));
}
void Store::check_delete(const Entry& n) const {
    writable(n);
    if (system_node(n->id) || n->parent == trash_id || (n->attributes & FILE_ATTRIBUTE_READONLY))
        throw WinError(ERROR_ACCESS_DENIED, "File cannot be deleted");
    if (n->directory()) {
        auto it = children_.find(n->id);
        if (it != children_.end() && !it->second.empty())
            throw WinError(ERROR_DIR_NOT_EMPTY, "Directory is not empty");
    }
}
void Store::can_delete(const Entry& previous) {
    std::lock_guard lock(mutex_);
    check_delete(latest(previous));
}

static Node move_to_trash(Node n) {
    if (n.original_parent.empty()) {
        n.original_parent = n.parent;
        n.original_name = n.name;
    }
    n.name = name_for_windows(n.original_name) + L"~" + wide(n.id.substr(0, 8));
    n.parent = trash_id;
    n.change = filetime();
    return n;
}

void Store::remove(const Entry& previous) {
    std::lock_guard lock(mutex_);
    auto n = latest(previous);
    if (n->parent == trash_id && previous->parent != trash_id) return;
    check_delete(n);
    auto moved = move_to_trash(*n);
    auto& siblings = children_[std::string(trash_id)];
    if (siblings.contains(moved.name)) moved.name = wide(n->id);
    transaction({std::move(moved)});
}

Entry Store::rename(const Entry& previous, std::wstring_view path, bool replace) {
    std::lock_guard lock(mutex_);
    auto source = latest(previous);
    writable(source);
    if (system_node(source->id)) throw WinError(ERROR_ACCESS_DENIED, "System directory cannot be renamed");
    auto [parent, name] = split_path(path);
    writable(parent);
    auto ancestor = parent;
    while (ancestor->id != root_id) {
        if (ancestor->id == source->id) throw WinError(ERROR_ACCESS_DENIED, "Directory cannot contain itself");
        ancestor = nodes_.at(ancestor->parent);
    }
    std::vector<Node> changes;
    if (auto it = children_[parent->id].find(name); it != children_[parent->id].end() && it->second != source->id) {
        if (!replace) throw WinError(ERROR_ALREADY_EXISTS, "Rename destination exists");
        const auto target = nodes_.at(it->second);
        if (target->directory() || source->directory() || target->parent == trash_id)
            throw WinError(ERROR_ACCESS_DENIED, "Cannot replace the destination");
        check_delete(target);
        auto moved = move_to_trash(*target);
        if (children_[std::string(trash_id)].contains(moved.name)) moved.name = wide(target->id);
        changes.push_back(std::move(moved));
    }
    Node n = *source;
    n.parent = parent->id;
    n.name = name;
    n.change = filetime();
    if (n.parent != trash_id) {
        n.original_parent.clear();
        n.original_name.clear();
    }
    changes.push_back(std::move(n));
    transaction(std::move(changes));
    return nodes_.at(source->id);
}

void Store::restore(std::wstring_view path) {
    std::lock_guard lock(mutex_);
    auto node = locate(path);
    writable(node);
    if (node->parent != trash_id) throw WinError(ERROR_INVALID_PARAMETER, "Select an entry from the volume trash");
    auto destination = nodes_.at(std::string(root_id));
    if (auto it = nodes_.find(node->original_parent); it != nodes_.end() && it->second->directory()) {
        auto ancestor = it->second;
        while (ancestor->id != root_id && ancestor->id != trash_id) ancestor = nodes_.at(ancestor->parent);
        if (ancestor->id == root_id) destination = it->second;
    }
    auto name = node->original_name.empty() ? node->name : node->original_name;
    if (children_[destination->id].contains(name))
        throw WinError(ERROR_ALREADY_EXISTS, "Restore destination exists. Move the entry to a different name");
    Node copy = *node;
    copy.name = std::move(name);
    copy.parent = destination->id;
    copy.original_name.clear();
    copy.original_parent.clear();
    copy.change = filetime();
    transaction({std::move(copy)});
}

void Store::mark(std::wstring_view path, bool favorite, bool value) {
    std::lock_guard lock(mutex_);
    auto node = locate(path);
    writable(node);
    if (system_node(node->id)) throw WinError(ERROR_ACCESS_DENIED, "Metadata of this directory is fixed");
    Node copy = *node;
    (favorite ? copy.favorite : copy.archived) = value;
    copy.change = filetime();
    transaction({std::move(copy)});
}

std::vector<std::wstring> Store::marked(bool favorite) const {
    std::lock_guard lock(mutex_);
    std::vector<std::wstring> result;
    for (const auto& [id, node] : nodes_) {
        (void)id;
        if (!(favorite ? node->favorite : node->archived)) continue;
        auto item = node;
        std::wstring path;
        while (item->id != root_id) {
            path.insert(0, L"\\" + item->name);
            item = nodes_.at(item->parent);
        }
        result.push_back(config_.mount + path);
    }
    std::ranges::sort(result, OrdinalLess{});
    return result;
}

Entry Store::set_basic(const Entry& previous, uint32_t attr, uint64_t c, uint64_t a, uint64_t m, uint64_t w) {
    std::lock_guard lock(mutex_);
    auto source = latest(previous);
    writable(source);
    if (system_node(source->id)) throw WinError(ERROR_ACCESS_DENIED, "System directory metadata is fixed");
    Node n = *source;
    if (attr != UINT32_MAX) {
        if (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ENCRYPTED))
            throw WinError(ERROR_NOT_SUPPORTED, "Unsupported requested file attributes");
        n.attributes = normalized_attributes(attr, n.directory());
    }
    if (c && c != UINT64_MAX) n.creation = c;
    if (a && a != UINT64_MAX) n.access = a;
    if (m && m != UINT64_MAX) n.modified = m;
    if (w && w != UINT64_MAX) n.change = w;
    transaction({std::move(n)});
    return nodes_.at(source->id);
}

void Store::resize_node(Node& n, uint64_t size) {
    if (size > file_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Windows file size limit");
    if (size < n.size) {
        n.blocks.erase(n.blocks.lower_bound((size + block_size - 1) / block_size), n.blocks.end());
        if (size % block_size) {
            auto last = n.blocks.find(size / block_size);
            if (last != n.blocks.end()) {
                auto original = native_block(last->second, normal_priority);
                if (original->size() > size % block_size) {
                    Bytes short_block(original->begin(), original->begin() + static_cast<ptrdiff_t>(size % block_size));
                    auto id = stage_block(std::move(short_block));
                    if (id.empty()) n.blocks.erase(last); else last->second = std::move(id);
                }
            }
        }
    }
    n.size = size;
    n.allocation = std::max(n.allocation, round_allocation(size));
}

Entry Store::resize(const Entry& previous, uint64_t size, bool allocate, uint32_t attrs) {
    std::lock_guard lock(mutex_);
    auto source = latest(previous);
    writable(source);
    if (source->directory()) throw WinError(ERROR_ACCESS_DENIED, "Cannot resize a directory");
    if (size > file_limit) throw WinError(ERROR_FILE_TOO_LARGE, "Allocation exceeds Windows file limit");
    Node n = *source;
    if (attrs != UINT32_MAX) {
        n.attributes = normalized_attributes(attrs, false);
        n.blocks.clear();
        n.size = 0;
    }
    if (!allocate || size < n.size) resize_node(n, size);
    n.allocation = allocate ? round_allocation(size) : round_allocation(n.size);
    n.change = n.modified = filetime();
    transaction({std::move(n)});
    return nodes_.at(source->id);
}

size_t Store::read(const Entry& previous, uint64_t offset, std::span<uint8_t> out) {
    auto n = current(previous);
    if (n->directory()) throw WinError(ERROR_ACCESS_DENIED, "Cannot read a directory");
    if (offset >= n->size) return 0;
    auto count = static_cast<size_t>(std::min<uint64_t>(out.size(), n->size - offset));
    size_t done = 0;
    while (done < count) {
        const auto pos = offset + done;
        const auto in_block = static_cast<size_t>(pos % block_size);
        const auto take = std::min<size_t>(count - done, block_size - in_block);
        auto data = block(*n, pos / block_size, normal_priority);
        auto destination = out.subspan(done, take);
        auto available = in_block >= data->size() ? 0 : std::min(take, data->size() - in_block);
        if (available) std::copy_n(data->data() + in_block, available, destination.data());
        std::fill(destination.begin() + static_cast<ptrdiff_t>(available), destination.end(), 0);
        done += take;
    }
    return count;
}

Entry Store::write(const Entry& previous, uint64_t offset, std::span<const uint8_t> input,
    bool append, bool constrained, size_t& count) {
    std::unique_lock lock(mutex_);
    for (;;) {
        auto source = latest(previous);
        writable(source);
        if (source->directory()) throw WinError(ERROR_ACCESS_DENIED, "Cannot write a directory");
        const auto position = append ? source->size : offset;
        auto take = input.size();
        if (constrained) {
            take = position >= source->size ? 0 :
                static_cast<size_t>(std::min<uint64_t>(take, source->size - position));
        }
        if (!take) { count = 0; return source; }
        if (position > file_limit || take > file_limit - position)
            throw WinError(ERROR_FILE_TOO_LARGE, "Windows file size limit");

        lock.unlock();
        Node n = *source;
        std::vector<std::pair<uint64_t, Bytes>> edits;
        size_t done = 0;
        while (done < take) {
            const auto pos = position + done;
            const auto index = pos / block_size;
            const auto in_block = static_cast<size_t>(pos % block_size);
            const auto size = std::min<size_t>(take - done, block_size - in_block);
            Bytes data;
            const auto old = n.blocks.find(index);
            if (size < block_size && old != n.blocks.end()) data = *native_block(old->second, normal_priority);
            if (data.size() < in_block + size) data.resize(in_block + size);
            std::copy_n(input.data() + done, size, data.data() + in_block);
            edits.emplace_back(index, std::move(data));
            done += size;
        }
        lock.lock();
        if (latest(previous) != source) continue;
        if (free_space() < input.size() + (2 * block_size)) gc_pending();
        for (auto& [index, data] : edits) {
            auto id = stage_block(std::move(data));
            if (id.empty()) n.blocks.erase(index); else n.blocks[index] = std::move(id);
        }
        n.size = std::max(n.size, position + take);
        n.allocation = std::max(n.allocation, round_allocation(n.size));
        n.change = n.modified = filetime();
        n.attributes = normalized_attributes(n.attributes | FILE_ATTRIBUTE_ARCHIVE, false);
        transaction({std::move(n)});
        count = done;
        return nodes_.at(source->id);
    }
}

uint64_t Store::free_space() const {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(dir_.c_str(), &available, nullptr, nullptr))
        throw WinError(GetLastError(), "Query local cache capacity");
    std::lock_guard lock(cache_mutex_);
    uint64_t pins = 0;
    for (const auto& [key, cached] : cache_) {
        (void)key;
        if (cached.pending) pins += cached.disk_size;
    }
    return std::min<uint64_t>(available.QuadPart, config_.disk > pins ? config_.disk - pins : 0);
}

void Store::trim_cache(uint64_t incoming) {
    if (incoming > config_.disk) throw WinError(ERROR_DISK_FULL, "A block exceeds the local cache limit");
    if (disk_bytes_ <= config_.disk - incoming) return;
    std::vector<std::pair<uint64_t, std::string>> removable;
    for (const auto& [key, c] : cache_)
        if (c.disk_size && !c.pending) removable.emplace_back(c.access, key);
    std::ranges::sort(removable);
    for (const auto& [last_used, key] : removable) {
        (void)last_used;
        if (disk_bytes_ <= config_.disk - incoming) break;
        auto it = cache_.find(key);
        if (!DeleteFileW(cache_file(key).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) continue;
        disk_bytes_ -= it->second.disk_size;
        it->second.disk_size = 0;
        if (!it->second.data) cache_.erase(it);
    }
    if (incoming && disk_bytes_ > config_.disk - incoming)
        throw WinError(ERROR_DISK_FULL, "Local cache is full of uncommitted blocks");
}

void Store::remember(const std::string& key, std::shared_ptr<const Bytes> bytes, uint64_t disk, bool pending) {
    auto& cached = cache_[key];
    cached.access = ++cache_tick_;
    cached.pending = cached.pending || pending;
    disk_bytes_ += disk;
    cached.disk_size += disk;
    if (cached.data || bytes->size() > config_.memory || !config_.memory) return;
    while (memory_bytes_ > config_.memory - bytes->size()) {
        auto oldest = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.data && (oldest == cache_.end() || it->second.access < oldest->second.access))
                oldest = it;
        }
        if (oldest == cache_.end()) break;
        memory_bytes_ -= oldest->second.data->size();
        oldest->second.data.reset();
        if (!oldest->second.disk_size) cache_.erase(oldest);
    }
    cached.data = std::move(bytes);
    memory_bytes_ += cached.data->size();
}

void Store::gc_pending() {
    std::set<std::string> needed;
    auto mark = [&](const Node& node) {
        for (const auto& [index, key] : node.blocks) {
            (void)index;
            needed.insert(key);
        }
    };
    for (const auto& [id, node] : nodes_) {
        (void)id;
        mark(*node);
    }
    std::erase_if(retired_, [&](const auto& weak) {
        if (auto node = weak.lock()) { mark(*node); return false; }
        return true;
    });
    std::lock_guard lock(cache_mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.pending && !needed.contains(it->first)) {
            if (!DeleteFileW(cache_file(it->first).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                ++it;
                continue;
            }
            disk_bytes_ -= it->second.disk_size;
            if (it->second.data) memory_bytes_ -= it->second.data->size();
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string Store::stage_block(Bytes bytes) {
    while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
    if (bytes.empty()) return {};
    const auto key = digest(bytes, key_.bytes);
    bool remote = false;
    {
        std::lock_guard lock(refs_mutex_);
        remote = refs_.contains(key);
    }
    std::lock_guard lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second.disk_size) return key;
    if (remote) {
        remember(key, std::make_shared<Bytes>(std::move(bytes)), 0, false);
        return key;
    }
    auto stored = wrap(bytes, "block:" + key);
    trim_cache(stored.size());
    atomic_write(cache_file(key), stored);
    remember(key, std::make_shared<Bytes>(std::move(bytes)), stored.size(), true);
    return key;
}

std::shared_ptr<const Bytes> Store::native_block(const std::string& key, int priority) {
    std::lock_guard guard(block_mutexes_[std::hash<std::string>{}(key) % block_mutexes_.size()]);
    const auto path = cache_file(key);
    {
        std::lock_guard lock(cache_mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) {
            it->second.access = ++cache_tick_;
            if (it->second.data) return it->second.data;
            if (it->second.disk_size) {
                try {
                    auto bytes = unwrap(read_file(path, block_size + 64), "block:" + key);
                    if (digest(bytes, key_.bytes) != key) throw WinError(ERROR_CRC, "Cached block checksum mismatch");
                    auto result = std::make_shared<Bytes>(std::move(bytes));
                    remember(key, result, 0, false);
                    return result;
                } catch (const std::exception& err) {
                    if (it->second.pending) throw;
                    log(config_, err.what());
                    auto size = it->second.disk_size;
                    if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                        disk_bytes_ -= size;
                        cache_.erase(it);
                    }
                }
            }
        }
    }
    Doc reference;
    {
        std::lock_guard lock(refs_mutex_);
        auto it = refs_.find(key);
        if (it == refs_.end()) throw WinError(ERROR_CRC, "Local uncommitted block is unavailable");
        reference = it->second;
    }
    if (reference.size > block_size + 64) throw WinError(ERROR_FILE_CORRUPT, "Remote block size is invalid");
    auto stored = tg_.download(reference, 0, static_cast<size_t>(reference.size), priority);
    auto bytes = unwrap(stored, "block:" + key);
    if (bytes.empty() || bytes.size() > block_size || digest(bytes, key_.bytes) != key)
        throw WinError(ERROR_CRC, "Remote block checksum mismatch");
    auto result = std::make_shared<Bytes>(std::move(bytes));
    std::lock_guard lock(cache_mutex_);
    uint64_t disk_size = 0;
    try {
        if (auto entry = cache_.find(key); entry == cache_.end() || !entry->second.disk_size) {
            trim_cache(stored.size());
            atomic_write(path, stored);
            disk_size = stored.size();
        }
    } catch (const std::exception& error) { log(config_, error.what(), false); }
    remember(key, result, disk_size, false);
    return result;
}

std::shared_ptr<const Bytes> Store::block(const Node& node, uint64_t index, int priority) {
    if (!node.legacy_chat) {
        auto it = node.blocks.find(index);
        if (it != node.blocks.end()) return native_block(it->second, priority);
        static const auto zero = std::make_shared<const Bytes>();
        return zero;
    }
    if (node.incomplete) throw WinError(ERROR_FILE_CORRUPT, "Missing DriveInfinity parts");
    const auto key = local_id(node, index);
    const auto path = cache_file(key);
    std::lock_guard guard(block_mutexes_[std::hash<std::string>{}(key) % block_mutexes_.size()]);
    {
        std::lock_guard lock(cache_mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) {
            it->second.access = ++cache_tick_;
            if (it->second.data) return it->second.data;
            if (it->second.disk_size) {
                try {
                    auto plain = dpapi(read_file(path, block_size + 4096), true);
                    if (plain.size() != std::min(block_size, node.size - index * block_size))
                        throw WinError(ERROR_CRC, "Cached DriveInfinity block size mismatch");
                    auto result = std::make_shared<Bytes>(std::move(plain));
                    remember(key, result, 0, false);
                    return result;
                } catch (const std::exception& err) {
                    log(config_, err.what());
                    if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                        disk_bytes_ -= it->second.disk_size;
                        cache_.erase(it);
                    }
                }
            }
        }
    }
    const auto secret = node.encrypted ? load_secret(legacy_key_path(config_, node.legacy_chat)) : Secret{};
    auto result = std::make_shared<Bytes>(read_legacy_block(tg_, node, index, secret.bytes, priority));
    auto stored = dpapi(*result, false);
    std::lock_guard lock(cache_mutex_);
    uint64_t disk_size = 0;
    try {
        if (auto entry = cache_.find(key); entry == cache_.end() || !entry->second.disk_size) {
            trim_cache(stored.size());
            atomic_write(path, stored);
            disk_size = stored.size();
        }
    } catch (const std::exception& error) { log(config_, error.what(), false); }
    remember(key, result, disk_size, false);
    return result;
}

void Store::prefetch(const Entry& node, uint64_t offset, unsigned count) {
    if (!config_.prefetch || offset >= node->size) return;
    auto index = (offset + block_size - 1) / block_size;
    std::lock_guard lock(wake_mutex_);
    for (unsigned i = 0; i < count && queue_.size() < 16 && index * block_size < node->size; ++i, ++index) {
        if (!std::ranges::any_of(queue_, [&](const auto& job) { return job.first == node && job.second == index; }))
            queue_.emplace_back(node, index);
    }
    prefetch_cv_.notify_all();
}

void Store::run_prefetch(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::pair<Entry, uint64_t> task;
        {
            std::unique_lock lock(wake_mutex_);
            prefetch_cv_.wait_for(lock, 250ms, [&] { return stop.stop_requested() || !queue_.empty(); });
            if (stop.stop_requested() || queue_.empty()) continue;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try { (void)block(*task.first, task.second, 8); }
        catch (const std::exception& e) { log(config_, e.what(), false); }
    }
}

std::optional<Store::Commit> Store::remote_index() {
    std::optional<Commit> result;
    const auto inspect = [&](const Doc& doc) {
        if (doc.name != "telegramfsindex.bin" || !doc.caption.starts_with(index_tag)) return true;
        const auto base = std::string_view(doc.caption).substr(index_tag.size());
        auto delimiter = base.find(':');
        if (delimiter == std::string_view::npos) throw WinError(ERROR_FILE_CORRUPT, "Invalid index caption");
        auto id = base.substr(0, delimiter);
        auto parent = base.substr(delimiter + 1);
        if (!object_id(id) || (!parent.empty() && !object_id(parent)) || id == parent || doc.size > metadata_limit)
            throw WinError(ERROR_FILE_CORRUPT, "Invalid index identity");
        result = Commit{std::string(id), std::string(parent), doc};
        return false;
    };
    tg_.search(config_.channel, "telegramfsindex", inspect);
    if (!result && !initialized_) tg_.search(config_.channel, "", inspect);
    return result;
}

Entries Store::decode_index(const Json& snapshot, const Commit& commit) const {
    if (!object_id(commit.id) || (!commit.parent.empty() && !object_id(commit.parent)) ||
        snapshot.at("v") != 1 || snapshot.at("id") != commit.id || snapshot.at("parent") != commit.parent)
        throw WinError(ERROR_FILE_CORRUPT, "Index lineage mismatch");
    Entries entries;
    for (auto id : {root_id, trash_id, imports_id}) entries.emplace(id, nodes_.at(std::string(id)));
    for (auto& n : snapshot.at("nodes").get<std::vector<Node>>()) {
        if (!object_id(n.id) || n.legacy_chat || !n.parts.empty())
            throw WinError(ERROR_FILE_CORRUPT, "Invalid remote file record");
        auto entry = std::make_shared<Node>(std::move(n));
        if (!entries.emplace(entry->id, entry).second)
            throw WinError(ERROR_FILE_CORRUPT, "Duplicate ID in remote index");
    }
    const auto remote = snapshot.at("refs").get<std::unordered_map<std::string, Doc>>();
    for (const auto& [key, doc] : remote) {
        if (!is_digest(key) || doc.chat != config_.channel || doc.caption != std::string(block_tag) + key ||
            doc.size < 6 || doc.size > block_size + 64)
            throw WinError(ERROR_FILE_CORRUPT, "Invalid remote block index");
    }
    for (const auto& [id, entry] : entries) {
        (void)id;
        for (const auto& [i, key] : entry->blocks) {
            (void)i;
            if (commit.doc.message && !remote.contains(key))
                throw WinError(ERROR_FILE_CORRUPT, "Missing remote block index entry");
        }
    }
    validate_catalog(entries);
    return entries;
}

void Store::pull(const Commit& commit) {
    const auto raw = tg_.download(commit.doc, 0, static_cast<size_t>(commit.doc.size));
    const auto snapshot = Json::parse(unwrap(raw, "index:" + commit.id));
    std::lock_guard lock(mutex_);
    if (generation_ != committed_ || open_files_)
        throw WinError(ERROR_RETRY, "Open files or local writes deferred the remote catalog update");
    auto candidate = decode_index(snapshot, commit);
    for (const auto& [id, node] : nodes_) if (node->legacy_chat) candidate.emplace(id, node);
    const auto remote = snapshot.at("refs").get<std::unordered_map<std::string, Doc>>();
    {
        std::lock_guard references(refs_mutex_);
        for (const auto& [key, doc] : remote) refs_[key] = doc;
    }
    checkpoint(candidate, generation_, commit.id);
    previous_head_ = commit.parent;
    head_ = commit.id;
    for (const auto& [id, old] : nodes_) {
        (void)id;
        if (!old->blocks.empty() && old.use_count() > 1) retired_.push_back(old);
    }
    nodes_.clear();
    children_.clear();
    for (const auto& [id, node] : candidate) {
        (void)id;
        set_entry(node);
    }
    initialized_ = true;
}

void Store::replace_native(const Entries& entries) {
    Entries candidate = entries;
    for (const auto& [id, old] : nodes_) {
        if (old->legacy_chat) candidate.emplace(id, old);
        if (!old->blocks.empty() && old.use_count() > 1) retired_.push_back(old);
    }
    nodes_.clear();
    children_.clear();
    for (const auto& [id, node] : candidate) {
        (void)id;
        set_entry(node);
    }
}

void Store::rebase(const Commit& commit) {
    const auto blob = tg_.download(commit.doc, 0, static_cast<size_t>(commit.doc.size));
    const auto snapshot = Json::parse(unwrap(blob, "index:" + commit.id));
    std::lock_guard lock(mutex_);
    if (open_files_) throw WinError(ERROR_RETRY, "Close open files to reconcile independent revisions");
    if (config_.read_only) throw WinError(ERROR_WRITE_PROTECT, "Writable mount required to reconcile revisions");
    auto candidate = decode_index(snapshot, commit);
    auto received = snapshot.at("refs").get<std::unordered_map<std::string, Doc>>();
    // Keep the local directory tree available when there is no common writable head.
    auto group_name = L"Conflictos de TelegramFS " + wide(uuid());
    const auto group = uuid();
    candidate.emplace(group, std::make_shared<Node>(directory(group, std::string(root_id), group_name, 0)));
    const auto deleted = uuid();
    candidate.emplace(deleted, std::make_shared<Node>(directory(deleted, group, L".Papelera",
        FILE_ATTRIBUTE_HIDDEN)));
    std::unordered_map<std::string, std::string> translated;
    translated.emplace(root_id, group);
    translated.emplace(trash_id, deleted);
    for (const auto& [id, node] : nodes_) {
        if (!system_node(id) && !node->legacy_chat) translated.emplace(id, uuid());
    }
    for (const auto& [id, node] : nodes_) {
        if (system_node(id) || node->legacy_chat) continue;
        auto copy = std::make_shared<Node>(*node);
        copy->id = translated.at(id);
        copy->parent = translated.at(node->parent);
        copy->original_name.clear();
        copy->original_parent.clear();
        candidate.emplace(copy->id, copy);
    }
    validate_catalog(candidate);
    const uint64_t next = generation_ + 1;
    const auto journal = Json{{"v",1},{"head",commit.id},{"id",commit.id},{"parent",commit.parent},
        {"nodes",native_nodes(candidate)},{"refs",received}}.dump();
    if (journal.size() + 64 > metadata_limit)
        throw WinError(ERROR_FILE_TOO_LARGE, "Conflict catalog is too large. Pending files remain local");
    if (free_space() < journal.size() + handle_reserve)
        throw WinError(ERROR_DISK_FULL, "Insufficient local space to reconcile revisions");
    atomic_write(tx_file(next), wrap(bytes_of(journal), "journal:" + std::to_string(next)));
    {
        std::lock_guard references(refs_mutex_);
        for (auto& [key, doc] : received) refs_.try_emplace(key, std::move(doc));
    }
    previous_head_ = commit.parent;
    head_ = commit.id;
    last_rebase_ = generation_ = next;
    replace_native(candidate);
    erase_if_exists(dir_ / L"flight");
    erase_if_exists(dir_ / L"flight-data");
    due_ = Clock::now();
    log(config_, "Concurrent revisions preserved under " + utf8(group_name));
}

Doc Store::upload_block(const std::string& key) {
    {
        std::lock_guard refs(refs_mutex_);
        if (auto it = refs_.find(key); it != refs_.end()) return it->second;
    }
    const auto caption = std::string(block_tag) + key;
    const auto pending_file = dir_ / L"receipts" / wide(key + ".pending");
    auto bytes = native_block(key, normal_priority);
    const auto payload = wrap(*bytes, "block:" + key);
    Doc doc;
    if (fs::exists(pending_file)) {
        tg_.search(config_.channel, key, [&](const Doc& candidate) {
            if (candidate.caption != caption || candidate.size != payload.size()) return true;
            doc = candidate;
            return false;
        });
    } else {
        atomic_write(pending_file, std::string_view("send"));
    }
    if (!doc.message) {
        StagingFile file(dir_ / L"stage", block_name);
        atomic_write(file.path, payload);
        doc = tg_.send_document(config_.channel, file.path, caption);
    }
    if (doc.size != payload.size() || doc.caption != caption)
        throw WinError(ERROR_CRC, "Telegram upload acknowledgment mismatch");
    // Verify the stored bytes before making a block eligible for eviction.
    const auto confirmed = tg_.download(doc, 0, static_cast<size_t>(doc.size));
    if (digest(unwrap(confirmed, "block:" + key), key_.bytes) != key)
        throw WinError(ERROR_CRC, "Uploaded block checksum mismatch");
    atomic_write(dir_ / L"receipts" / wide(key), wrap(bytes_of(Json(doc).dump()), "receipt:" + key));
    {
        std::lock_guard refs(refs_mutex_);
        refs_[key] = doc;
    }
    {
        std::lock_guard lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) it->second.pending = false;
    }
    erase_if_exists(pending_file);
    return doc;
}

void Store::publish(Entries entries, uint64_t generation, const std::string& parent) {
    std::set<std::string> needed;
    for (const auto& [id, entry] : entries) {
        (void)id;
        for (const auto& [i, key] : entry->blocks) {
            (void)i;
            needed.insert(key);
        }
    }
    const std::vector<std::string> keys(needed.begin(), needed.end());
    std::atomic<size_t> next = 0;
    std::atomic<bool> failed = false;
    std::mutex output;
    std::exception_ptr error;
    std::unordered_map<std::string, Doc> refs;
    std::vector<std::jthread> threads;
    for (size_t i = 0; i < std::min<size_t>(config_.uploads, keys.size()); ++i) {
        threads.emplace_back([&] {
            while (!failed) {
                const auto index = next++;
                if (index >= keys.size()) break;
                try {
                    auto receipt = upload_block(keys[index]);
                    std::lock_guard lock(output);
                    refs.emplace(keys[index], std::move(receipt));
                } catch (...) {
                    std::lock_guard lock(output);
                    if (!error) error = std::current_exception();
                    failed = true;
                }
            }
        });
    }
    threads.clear();
    if (error) std::rethrow_exception(error);

    const auto id = uuid();
    const auto text = Json{{"v",1},{"id",id},{"parent",parent},{"nodes",native_nodes(entries)},
        {"refs",std::move(refs)}}.dump();
    if (text.size() + 64 > metadata_limit)
        throw WinError(ERROR_FILE_TOO_LARGE, "Remote index exceeds 64 MiB. Pending data retained locally");
    atomic_write(dir_ / L"flight-data", wrap(bytes_of(text), "index:" + id));
    const auto flight = Json{{"id",id},{"parent",parent},{"g",generation}}.dump();
    atomic_write(dir_ / L"flight", wrap(bytes_of(flight), "flight"));
}

void Store::finish_flight(const std::optional<Commit>& remote) {
    const auto flight = Json::parse(unwrap(read_file(dir_ / L"flight", 4096), "flight"));
    const auto id = flight.at("id").get<std::string>();
    const auto parent = flight.at("parent").get<std::string>();
    const auto g = flight.at("g").get<uint64_t>();
    {
        std::lock_guard lock(mutex_);
        if (g <= committed_ && head_ == id) {
            erase_if_exists(dir_ / L"flight");
            erase_if_exists(dir_ / L"flight-data");
            return;
        }
        if (parent != head_ || g > generation_ || g <= committed_)
            throw WinError(ERROR_FILE_CORRUPT, "Invalid local upload journal");
    }
    auto stored = read_file(dir_ / L"flight-data", metadata_limit);
    const auto snapshot = Json::parse(unwrap(stored, "index:" + id));
    auto confirmed = remote && remote->id == id ? remote->doc : Doc{};
    if (!confirmed.message) {
        if (parent != (remote ? remote->id : std::string{}))
            throw WinError(ERROR_REVISION_MISMATCH, "Remote index advanced during a pending local commit");
        StagingFile file(dir_ / L"stage", index_name);
        atomic_write(file.path, stored);
        const auto caption = std::string(index_tag) + id + ":" + parent;
        confirmed = tg_.send_document(config_.channel, file.path, caption);
        if (confirmed.caption != caption || confirmed.size != stored.size())
            throw WinError(ERROR_CRC, "Index upload acknowledgment mismatch");
    }
    const auto checked = tg_.download(confirmed, 0, static_cast<size_t>(confirmed.size));
    if (stored != checked) throw WinError(ERROR_CRC, "Uploaded index verification failed");
    std::lock_guard lock(mutex_);
    auto published = decode_index(snapshot, Commit{id, parent, confirmed});
    checkpoint(published, g, id);
    committed_ = g;
    previous_head_ = parent;
    head_ = id;
    discard_journal(g);
    initialized_ = true;
    erase_if_exists(dir_ / L"flight");
    erase_if_exists(dir_ / L"flight-data");
    gc_pending();
}

void Store::sync() {
    std::lock_guard serial(sync_mutex_);
    auto remote = remote_index();
    std::string local_head;
    std::string former_head;
    bool is_dirty = false;
    {
        std::lock_guard lock(mutex_);
        local_head = head_;
        former_head = previous_head_;
        is_dirty = generation_ != committed_;
    }

    if (fs::exists(dir_ / L"flight")) {
        const auto flight = Json::parse(unwrap(read_file(dir_ / L"flight", 4096), "flight"));
        const auto target = flight.at("id").get<std::string>();
        if (flight.at("g").get<uint64_t>() < last_rebase_) {
            erase_if_exists(dir_ / L"flight");
            erase_if_exists(dir_ / L"flight-data");
        } else if (remote && remote->id != target && remote->id != local_head && remote->id != former_head) {
            rebase(*remote);
            return;
        } else if (config_.read_only) {
            throw WinError(ERROR_WRITE_PROTECT, "A local transaction is awaiting a writable mount");
        } else {
            finish_flight(remote);
            return;
        }
    }

    if (!remote && !local_head.empty())
        throw WinError(ERROR_RETRY, "The current remote index is temporarily unavailable");
    if (remote && !local_head.empty() && remote->id == former_head)
        throw WinError(ERROR_RETRY, "Waiting for Telegram to index the acknowledged revision");
    if (remote && remote->id != local_head) {
        bool descendant = local_head.empty() || local_head == remote->parent;
        if (!descendant) {
            auto ancestor = remote->parent;
            tg_.search(config_.channel, "telegramfsindex", [&](const Doc& doc) {
                if (ancestor == local_head) { descendant = true; return false; }
                if (doc.name != "telegramfsindex.bin" || !doc.caption.starts_with(index_tag)) return true;
                const auto caption = std::string_view(doc.caption).substr(index_tag.size());
                const auto colon = caption.find(':');
                if (colon != std::string_view::npos && caption.substr(0, colon) == ancestor)
                    ancestor = caption.substr(colon + 1);
                return !ancestor.empty();
            });
            descendant = descendant || ancestor == local_head;
        }
        if (is_dirty || !descendant) rebase(*remote);
        else pull(*remote);
        return;
    }

    Entries copy;
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        gc_pending();
        if (generation_ == committed_) {
            if (!initialized_) { checkpoint(nodes_, generation_, head_); initialized_ = true; }
            return;
        }
        if (config_.read_only) return;
        copy = nodes_;
        generation = generation_;
    }
    publish(std::move(copy), generation, local_head);
    const auto latest = remote_index();
    if (latest && latest->id != local_head) {
        rebase(*latest);
    } else {
        finish_flight(latest);
    }
}

void Store::refresh_now() {
    sync();
    for (const auto chat : config_.imports) tg_.verify_channel(chat, false);
    rebuild_imports();
}

void Store::start() {
    if (!initialized_) sync();
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    if (config_.prefetch && config_.downloads > 1) {
        for (unsigned i = 0; i < std::min(config_.downloads - 1, 3u); ++i)
            prefetchers_.emplace_back([this](std::stop_token stop) { run_prefetch(stop); });
    }
}

void Store::stop() {
    if (!worker_.joinable() && prefetchers_.empty()) return;
    worker_.request_stop();
    for (auto& thread : prefetchers_) thread.request_stop();
    tg_.stop_io();
    wake_cv_.notify_all();
    prefetch_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    prefetchers_.clear();
}

bool Store::dirty() const {
    std::lock_guard lock(mutex_);
    return generation_ != committed_;
}

bool Store::flush(bool force_remote) {
    if (force_remote && dirty()) sync();
    return !dirty();
}

void Store::wake() { wake_cv_.notify_one(); }

void Store::request_sync() {
    refresh_requested_ = true;
    wake();
}

void Store::open_file() {
    std::lock_guard lock(mutex_);
    ++open_files_;
}

void Store::close_file() {
    std::lock_guard lock(mutex_);
    --open_files_;
    wake();
}

void Store::run(std::stop_token stop) {
    auto next_poll = Clock::now();
    auto next_import = Clock::now();
    auto retry = Clock::now();
    unsigned failures = 0;
    uint64_t update = tg_.revision();
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (refresh_requested_.exchange(false)) {
            next_poll = now;
            next_import = now;
            retry = now;
        }
        bool due = false;
        {
            std::lock_guard lock(mutex_);
            due = generation_ != committed_ && (now >= due_ || now >= next_poll);
        }
        if (now >= retry && (due || now >= next_poll || (tg_.revision() != update && now + 2s >= next_poll))) {
            try {
                sync();
                failures = 0;
                next_poll = Clock::now() + std::chrono::seconds(config_.poll);
            } catch (const std::exception& e) {
                log(config_, e.what());
                const unsigned seconds = std::min(120u, 1u << std::min(++failures, 7u));
                retry = Clock::now() + std::chrono::seconds(seconds);
            }
            update = tg_.revision();
        }
        if (!config_.imports.empty() && now >= next_import && !stop.stop_requested()) {
            try {
                for (const auto chat : config_.imports) tg_.verify_channel(chat, false);
                rebuild_imports();
            } catch (const std::exception& e) { log(config_, e.what()); }
            next_import = Clock::now() + std::chrono::seconds(config_.poll);
        }
        std::unique_lock lock(wake_mutex_);
        wake_cv_.wait_for(lock, 350ms);
    }
}
}
