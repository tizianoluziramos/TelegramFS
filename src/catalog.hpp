#pragma once
#include "telegram.hpp"
#include <map>

namespace tfs {
inline constexpr uint64_t block_size = 1 << 20;
inline constexpr uint64_t file_limit = INT64_MAX - block_size;
inline constexpr uint64_t metadata_limit = 64ull << 20;
inline constexpr std::string_view root_id = "root";
inline constexpr std::string_view trash_id = "trash";
inline constexpr std::string_view imports_id = "imports";

struct Part {
    uint64_t offset = 0;
    uint64_t length = 0;
    Doc doc;
};

struct Node {
    std::string id;
    std::string parent;
    std::wstring name;
    uint64_t size = 0;
    uint64_t allocation = 0;
    uint64_t creation = 0, access = 0, modified = 0, change = 0;
    uint32_t attributes = FILE_ATTRIBUTE_NORMAL;
    std::string original_parent;
    std::wstring original_name;
    std::map<uint64_t, std::string> blocks;
    std::vector<Part> parts;
    int64_t legacy_chat = 0;
    bool encrypted = false;
    bool favorite = false;
    bool archived = false;
    bool incomplete = false;
    bool directory() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};
using Entry = std::shared_ptr<const Node>;
using Entries = std::unordered_map<std::string, Entry>;

void to_json(Json&, const Node&);
void from_json(const Json&, Node&);
void to_json(Json&, const Part&);
void from_json(const Json&, Part&);
Node directory(std::string id, std::string parent, std::wstring name, uint32_t attributes);
std::string stable_id(std::string_view data);
std::vector<Node> read_legacy(const Config&, Telegram&, int64_t chat, bool regular_documents);
Bytes read_legacy_block(Telegram&, const Node&, uint64_t index, std::span<const uint8_t>, int priority);
fs::path legacy_key_path(const Config&, int64_t chat);
}
