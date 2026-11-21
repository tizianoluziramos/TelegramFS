#include "catalog.hpp"
#include "crypto.hpp"
#include <set>

namespace tfs {
namespace {
struct LegacyFile {
    Doc main;
    Json manifest;
    std::map<int, Part> parts;
};

bool internal(std::string_view name, std::string_view caption) {
    return caption.starts_with("#telegramfs-") || caption.starts_with(legacy_folders_tag)
        || caption.starts_with(legacy_key_tag) || caption.starts_with("#driveinfinity-icon")
        || name == "driveinfinity.folders.json" || name == "driveinfinity.keybackup";
}

Json parse_manifest(const Doc& doc, const Secret& key) {
    if (doc.caption.starts_with("td1:")) return Json::parse(doc.caption.substr(4));
    if (doc.caption.starts_with("tde1:")) {
        if (key.bytes.empty()) throw std::runtime_error("DriveInfinity encryption key is required");
        return Json::parse(unseal(key.bytes, decode_base64(std::string_view(doc.caption).substr(5))));
    }
    return Json{};
}

std::string nullable(const Json& value, const char* key) {
    auto it = value.find(key);
    return it == value.end() || it->is_null() ? std::string{} : it->get<std::string>();
}

uint64_t nonnegative(const Json& value, const char* key, uint64_t fallback = 0) {
    auto it = value.find(key);
    if (it == value.end()) return fallback;
    if (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<int64_t>() < 0))
        throw WinError(ERROR_INVALID_DATA, "Invalid DriveInfinity size or offset");
    return it->get<uint64_t>();
}

void legacy_metadata(Node& n, const Json& j, int64_t timestamp) {
    n.creation = from_unix_ms(j.value("ct", timestamp));
    n.modified = n.access = n.change = from_unix_ms(j.value("mt", timestamp));
    n.favorite = j.value("fv", false);
    n.archived = j.value("ar", false);
    if (j.value("hd", false)) n.attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (n.archived) n.attributes |= FILE_ATTRIBUTE_ARCHIVE;
}

bool trashed(const Json& j) {
    auto it = j.find("tr");
    return it != j.end() && !it->is_null();
}

struct Tree {
    int64_t chat;
    std::string domain;
    std::string root;
    std::string trash;
    std::map<std::string, Node> nodes;
    std::map<std::string, std::map<std::wstring, std::string, OrdinalLess>> names;

    explicit Tree(int64_t channel)
        : chat(channel), domain(std::to_string(chat)), root(stable_id(domain + "/legacy")),
          trash(stable_id(domain + "/trash")) {
        add(directory(root, std::string(imports_id), L"Canal " + wide(domain), FILE_ATTRIBUTE_READONLY));
        add(directory(trash, root, L".Papelera", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY));
    }

    Node& add(Node node) {
        node.legacy_chat = chat;
        auto& siblings = names[node.parent];
        auto original = node.name;
        unsigned sequence = 0;
        while (siblings.contains(node.name)) {
            node.name = original + L"~" + wide(node.id.substr(0, 8));
            if (++sequence > 1) node.name += L"-" + std::to_wstring(sequence);
        }
        siblings.emplace(node.name, node.id);
        return nodes.emplace(node.id, std::move(node)).first->second;
    }

    std::string folder(std::string_view full_path) {
        auto parent = root;
        size_t depth = 0;
        while (!full_path.empty()) {
            if (++depth > 256) throw std::runtime_error("DriveInfinity path is too deep");
            const auto slash = full_path.find('/');
            const auto raw = full_path.substr(0, slash);
            auto name = name_for_windows(wide(raw));
            if (auto it = names[parent].find(name); it != names[parent].end() && nodes.at(it->second).directory()) {
                parent = it->second;
            } else {
                auto id = stable_id(domain + "/path/" + parent + "/" + std::string(raw));
                parent = add(directory(id, parent, std::move(name), FILE_ATTRIBUTE_READONLY)).id;
            }
            if (slash == std::string_view::npos) break;
            full_path.remove_prefix(slash + 1);
        }
        return parent;
    }
};
}

fs::path legacy_key_path(const Config& config, int64_t chat) {
    return config.root / (L"legacy-key-" + std::to_wstring(chat) + L".dpapi");
}

std::vector<Node> read_legacy(const Config& config, Telegram& tg, int64_t chat, bool regular_documents) {
    const auto path = legacy_key_path(config, chat);
    const auto key = fs::exists(path) ? load_secret(path) : Secret{};
    Doc folder_doc;
    std::map<std::string, LegacyFile> files;
    size_t locked = 0;
    std::set<std::string> unique;
    Tree tree(chat);
    auto& folders = tree.nodes;
    tg.search(chat, "", [&](const Doc& doc) {
        if (doc.caption.starts_with(legacy_folders_tag) || doc.name == "driveinfinity.folders.json") {
            if (!folder_doc.message) folder_doc = doc;
            return true;
        }
        if (internal(doc.name, doc.caption)) return true;
        if (doc.caption.starts_with("tde1:") && key.bytes.empty()) {
            ++locked;
            return true;
        }
        try {
            const auto manifest = parse_manifest(doc, key);
            if (manifest.is_null()) {
                if (!regular_documents || (!doc.unique.empty() && !unique.insert(doc.unique).second)) return true;
                const std::string id = "message/" + std::to_string(doc.message);
                files[id] = {doc, Json::object(), {}};
                return true;
            }
            if (manifest.value("v", 1) < 1 || manifest.value("v", 1) > 2)
                throw std::runtime_error("Unknown DriveInfinity manifest version");
            const auto id = manifest.at("id").get<std::string>();
            auto& file = files["file/" + id];
            const int count = manifest.value("pc", 0);
            const int part = manifest.value("pi", 0);
            if (count > 1) {
                if (part < 0 || part >= count) throw std::runtime_error("Invalid DriveInfinity part index");
                const auto offset = nonnegative(manifest, "po");
                const auto length = nonnegative(manifest, "ps");
                file.parts.try_emplace(part, Part{offset, length, doc});
            }
            const bool plain = count <= 1 && file.parts.empty();
            if (!file.main.message || ((part == 0 || plain) &&
                (!file.manifest.is_object() || file.manifest.value("pi", 0) != 0))) {
                file.main = doc;
                file.manifest = manifest;
            }
        } catch (const std::exception& e) {
            log(config, "DriveInfinity message " + std::to_string(doc.message) + ": " + e.what());
        }
        return true;
    });

    if (locked)
        log(config, "DriveInfinity encrypted documents skipped. Run telegramfs key-restore --legacy " +
            std::to_string(chat) + " (" + std::to_string(locked) + " documents)");
    if (folder_doc.message && folder_doc.size <= metadata_limit) {
        auto bytes = tg.download(folder_doc, 0, static_cast<size_t>(folder_doc.size));
        std::span<const uint8_t> payload(bytes);
        if (bytes.size() >= 4 && string_of(payload.first(4)) == "TDFS") {
            if (key.bytes.empty()) {
                log(config, "DriveInfinity folder key is required", false);
                bytes.clear();
            } else {
                bytes = unseal(key.bytes, payload.subspan(4));
            }
        }
        if (!bytes.empty()) {
            const auto state = Json::parse(bytes);
            if (state.value("v", 1) != 1) throw std::runtime_error("Unknown DriveInfinity folder version");
            std::map<std::string, Json> pending;
            for (const auto& item : state.value("f", Json::array()))
                pending.try_emplace(item.at("id").get<std::string>(), item);
            std::set<std::string> visiting;
            const auto add_folder = [&](auto&& self, const std::string& id, unsigned depth) -> std::string {
                auto key_id = stable_id(tree.domain + "/folder/" + id);
                if (folders.contains(key_id)) return key_id;
                if (!pending.contains(id) || depth > 256 || !visiting.insert(id).second) return tree.root;
                Finally unvisit{[&] { visiting.erase(id); }};
                const auto& source = pending.at(id);
                auto parent_id = nullable(source, "p");
                auto parent = trashed(source) ? tree.trash :
                    parent_id.empty() ? tree.root : self(self, parent_id, depth + 1);
                Node n = directory(key_id, parent, name_for_windows(wide(source.at("n").get<std::string>())),
                    FILE_ATTRIBUTE_READONLY);
                legacy_metadata(n, source, folder_doc.modified_ms);
                tree.add(std::move(n));
                return key_id;
            };
            for (const auto& [id, unused] : pending) {
                (void)unused;
                add_folder(add_folder, id, 0);
            }
        }
    }

    for (auto& [id, file] : files) {
        try {
            const auto& m = file.manifest;
            const auto& d = file.main;
            if (!d.message) continue;
            auto parent = tree.root;
            if (trashed(m)) parent = tree.trash;
            else if (auto fid = nullable(m, "fid"); !fid.empty() &&
                folders.contains(stable_id(tree.domain + "/folder/" + fid)))
                parent = stable_id(tree.domain + "/folder/" + fid);
            else parent = tree.folder(m.value("p", std::string{}));
            Node n;
            n.id = stable_id(tree.domain + "/" + id);
            n.parent = parent;
            n.name = name_for_windows(wide(m.value("n", d.name)));
            n.size = nonnegative(m, "s", d.size);
            if (n.size > file_limit) throw std::runtime_error("DriveInfinity file exceeds the Windows size limit");
            n.attributes = FILE_ATTRIBUTE_READONLY;
            n.encrypted = m.value("e", false) || d.caption.starts_with("tde1:");
            legacy_metadata(n, m, d.modified_ms);
            if (!file.parts.empty()) {
                for (const auto& [index, part] : file.parts) {
                    (void)index;
                    n.parts.push_back(part);
                }
                std::ranges::sort(n.parts, {}, &Part::offset);
                uint64_t end = 0;
                for (const auto& part : n.parts) {
                    if (part.offset != end || !part.length || part.offset > n.size || part.length > n.size - part.offset) {
                        n.incomplete = true;
                        break;
                    }
                    end += part.length;
                }
                n.incomplete = n.incomplete || end != n.size;
            } else {
                n.parts = {Part{0, n.size, d}};
                n.incomplete = !n.encrypted && d.size < n.size;
            }
            tree.add(std::move(n));
        } catch (const std::exception& e) {
            log(config, "DriveInfinity entry " + id + ": " + e.what());
        }
    }
    std::vector<Node> result;
    for (auto& [id, node] : tree.nodes) {
        (void)id;
        result.push_back(std::move(node));
    }
    return result;
}

Bytes read_legacy_block(Telegram& tg, const Node& n, uint64_t index, std::span<const uint8_t> key,
    int priority) {
    if (n.incomplete) throw WinError(ERROR_FILE_CORRUPT, "DriveInfinity file has missing parts");
    const uint64_t start = index * block_size;
    const uint64_t end = start + std::min(block_size, n.size - start);
    Bytes result(static_cast<size_t>(end - start));
    auto it = std::ranges::upper_bound(n.parts, start, {}, &Part::offset);
    if (it == n.parts.begin()) throw WinError(ERROR_FILE_CORRUPT, "Missing DriveInfinity content");
    for (--it; it != n.parts.end() && it->offset < end; ++it) {
        const uint64_t from = std::max(start, it->offset) - it->offset;
        const uint64_t to = std::min(end, it->offset + it->length) - it->offset;
        if (to <= from) continue;
        auto out = result.begin() + static_cast<ptrdiff_t>(it->offset + from - start);
        if (!n.encrypted) {
            auto bytes = tg.download(it->doc, from, static_cast<size_t>(to - from), priority);
            std::copy(bytes.begin(), bytes.end(), out);
            continue;
        }
        const auto header = tg.download(it->doc, 0, 13, priority);
        for (uint64_t cursor = from; cursor < to;) {
            const auto frame = cursor / block_size;
            const auto frame_start = frame * block_size;
            const auto frame_length = std::min(block_size, it->length - frame_start);
            const uint64_t stored_offset = 13 + frame * (block_size + 20);
            auto stored = tg.download(it->doc, stored_offset, static_cast<size_t>(frame_length + 20), priority);
            const auto plain = legacy_frame(key, header, static_cast<uint32_t>(frame), stored,
                frame_start + frame_length == it->length);
            const size_t take = static_cast<size_t>(std::min(to - cursor, frame_start + frame_length - cursor));
            std::copy_n(plain.begin() + static_cast<ptrdiff_t>(cursor - frame_start), take, out);
            out += static_cast<ptrdiff_t>(take);
            cursor += take;
        }
    }
    return result;
}
}
