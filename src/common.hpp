#pragma once
#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#include <windows.h>
#undef WIN32_NO_STATUS
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tfs {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

struct WinError : std::runtime_error {
    DWORD code;
    explicit WinError(DWORD code, std::string message);
};

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& source) noexcept : value(std::exchange(source.value, nullptr)) {}
    Handle& operator=(Handle&& source) noexcept {
        if (this != &source) {
            if (*this) CloseHandle(value);
            value = std::exchange(source.value, nullptr);
        }
        return *this;
    }
    ~Handle() { if (*this) CloseHandle(value); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return value; }
};

struct Secret {
    Bytes bytes;
    Secret() = default;
    explicit Secret(Bytes value) : bytes(std::move(value)) {}
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&& value) noexcept : bytes(std::move(value.bytes)) {}
    ~Secret() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};

struct Config {
    fs::path root;
    std::wstring mount = L"T:";
    bool automatic = false;
    bool read_only = false;
    int64_t channel = 0;
    std::vector<int64_t> imports;
    unsigned downloads = 4;
    unsigned uploads = 4;
    unsigned poll = 60;
    uint64_t memory = 64ull << 20;
    uint64_t disk = 10ull << 30;
    bool prefetch = true;
    bool write_back = true;
    bool encryption = false;
    bool info_log = false;
};

struct OrdinalLess {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const;
};
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::wstring name_for_windows(std::wstring name);
void validate_name(std::wstring_view name);
std::wstring user_sid();
std::wstring read_password(std::wstring_view title);
std::wstring read_line(std::wstring_view title);
std::wstring mount_point(std::wstring value);
fs::path exe_path();
fs::path data_path();
Config load_config();
void log(const Config&, std::string_view message, bool error = true) noexcept;
Bytes read_file(const fs::path&, uint64_t maximum);
Bytes read_range(const fs::path&, uint64_t offset, size_t length);
void atomic_write(const fs::path&, std::span<const uint8_t>);
void atomic_write(const fs::path&, std::string_view);
void erase_if_exists(const fs::path&) noexcept;
uint64_t filetime();
uint64_t from_unix_ms(int64_t ms);
std::string uuid();
std::span<const uint8_t> bytes_of(std::string_view value);
std::string string_of(std::span<const uint8_t> value);
template<class F> struct Finally {
    F f;
    ~Finally() { f(); }
};
}
