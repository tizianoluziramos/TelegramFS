#pragma once
#include "common.hpp"

namespace tfs {
Bytes random_bytes(size_t size);
Bytes dpapi(std::span<const uint8_t> value, bool decrypt);
Secret load_secret(const fs::path&);
void save_secret(const fs::path&, std::span<const uint8_t>);
Bytes decode_base64(std::string_view input);
std::string encode_base64(std::span<const uint8_t>);
std::string digest(std::span<const uint8_t> data, std::span<const uint8_t> key = {});
bool is_digest(std::string_view value);
Bytes seal(std::span<const uint8_t> key, std::span<const uint8_t> plain, std::string_view aad = {});
Bytes unseal(std::span<const uint8_t> key, std::span<const uint8_t> sealed, std::string_view aad = {});
Bytes legacy_frame(std::span<const uint8_t> key, std::span<const uint8_t> header,
    uint32_t frame, std::span<const uint8_t> stored, bool last);
Bytes make_key_backup(std::span<const uint8_t> key, std::string_view password);
Secret restore_key_backup(std::span<const uint8_t> backup, std::string_view password);
uint32_t big_endian(std::span<const uint8_t>);
}
