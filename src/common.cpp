#include "common.hpp"
#include <sddl.h>
#include <shlobj.h>
#include <toml++/toml.hpp>
#include <cwctype>
#include <iostream>
#include <limits>
#include <mutex>

namespace tfs {
WinError::WinError(DWORD value, std::string message)
    : std::runtime_error(std::move(message) + " (Win32 " + std::to_string(value) + ")"), code(value) {}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw WinError(ERROR_INVALID_NAME, "Invalid UTF-16");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw WinError(ERROR_INVALID_NAME, "Invalid UTF-8");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

bool OrdinalLess::operator()(std::wstring_view a, std::wstring_view b) const {
    if (a.empty() || b.empty()) return a.size() < b.size();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
        static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

static bool invalid_char(wchar_t c) {
    return c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos;
}

static bool device_name(std::wstring_view name) {
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    const auto eq = [](std::wstring_view a, std::wstring_view b) {
        return !OrdinalLess{}(a, b) && !OrdinalLess{}(b, a);
    };
    if (eq(stem, L"CON") || eq(stem, L"PRN") || eq(stem, L"AUX") ||
        eq(stem, L"NUL") || eq(stem, L"CONIN$") || eq(stem, L"CONOUT$")) return true;
    return stem.size() == 4 && (eq(stem.substr(0, 3), L"COM") || eq(stem.substr(0, 3), L"LPT")) &&
        std::wstring_view(L"123456789¹²³").find(stem.back()) != std::wstring_view::npos;
}

void validate_name(std::wstring_view name) {
    if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
        std::ranges::any_of(name, invalid_char) || device_name(name)) {
        throw WinError(ERROR_INVALID_NAME, "Invalid Windows path component");
    }
    (void)utf8(name);
}

std::wstring name_for_windows(std::wstring name) {
    for (auto& c : name) if (invalid_char(c)) c = L'_';
    if (name.size() > 240) name.resize(240);
    if (!name.empty() && name.back() >= 0xD800 && name.back() <= 0xDBFF) name.pop_back();
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"_";
    if (device_name(name)) name.insert(name.begin(), L'_');
    return name;
}

std::wstring user_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw WinError(GetLastError(), "OpenProcessToken");
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    Bytes data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size))
        throw WinError(GetLastError(), "GetTokenInformation");
    PWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        throw WinError(GetLastError(), "ConvertSidToStringSid");
    Finally free{[&] { LocalFree(sid); }};
    return sid;
}

std::wstring read_line(std::wstring_view title) {
    std::wcerr << title << std::flush;
    std::wstring value;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        if (!std::getline(std::wcin, value)) throw std::runtime_error("Console input ended");
        return value;
    }
    std::array<wchar_t, 256> buffer{};
    Finally wipe{[&] { SecureZeroMemory(buffer.data(), sizeof(buffer)); }};
    while (value.size() < 4096) {
        DWORD count = 0;
        if (!ReadConsoleW(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count)
            throw std::runtime_error("Console input ended");
        for (DWORD i = 0; i < count; ++i) {
            if (buffer[i] == L'\n') return value;
            if (buffer[i] != L'\r') value += buffer[i];
        }
    }
    throw std::runtime_error("Console input exceeds 4096 characters");
}

std::wstring read_password(std::wstring_view title) {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    Finally restore{[&] {
        if (mode) SetConsoleMode(input, mode);
        std::wcerr << L'\n';
    }};
    return read_line(title);
}

std::wstring mount_point(std::wstring value) {
    if (value.size() != 2 || value[1] != L':' ||
        !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')))
        throw std::runtime_error("Expected a drive letter such as T:");
    value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    return value;
}

fs::path exe_path() {
    std::wstring result(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    if (!length || length == result.size()) throw WinError(GetLastError(), "GetModuleFileName");
    result.resize(length);
    return result;
}

fs::path data_path() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw std::runtime_error("LocalAppData is unavailable");
    Finally free{[&] { CoTaskMemFree(raw); }};
    fs::path result = fs::path(raw) / L"TelegramFS";
    fs::create_directories(result);
    return result;
}

Config load_config() {
    Config config;
    config.root = data_path();
    const auto path = config.root / L"telegramfs.toml";
    if (!fs::exists(path)) {
        const auto example = exe_path().parent_path() / L"telegramfs.toml";
        if (fs::exists(example)) atomic_write(path, read_file(example, 65536));
        else atomic_write(path, "[filesystem]\nmount_point = \"T:\"\n");
    }
    const auto document = toml::parse(string_of(read_file(path, 65536)));
    const auto v = [&](auto section, auto name, auto fallback) {
        auto node = document[section][name];
        if (!node) return fallback;
        auto value = node.template value<decltype(fallback)>();
        if (!value) throw std::runtime_error(std::string(section) + "." + name + " has the wrong type");
        return *value;
    };
    const auto count = [&](auto section, auto key, int64_t fallback, int64_t low, int64_t high) {
        auto result = v(section, key, fallback);
        if (result < low || result > high) throw std::runtime_error(std::string(key) + " is out of range");
        return result;
    };
    config.mount = mount_point(wide(v("filesystem", "mount_point", std::string("T:"))));
    config.automatic = v("filesystem", "auto_mount", false);
    config.read_only = v("filesystem", "read_only", false);
    config.channel = v("telegram", "channel_id", int64_t{0});
    if (auto list = document["telegram"]["import_channels"]; list) {
        const auto* array = list.as_array();
        if (!array) throw std::runtime_error("import_channels must be an array");
        for (const auto& item : *array) {
            const auto id = item.value<int64_t>();
            if (!id || *id >= 0) throw std::runtime_error("Invalid import channel ID");
            if (std::ranges::find(config.imports, *id) == config.imports.end()) config.imports.push_back(*id);
        }
    }
    config.downloads = static_cast<unsigned>(count("telegram", "parallel_downloads", 4, 1, 16));
    config.uploads = static_cast<unsigned>(count("telegram", "parallel_uploads", 4, 1, 16));
    config.poll = static_cast<unsigned>(count("telegram", "poll_seconds", 60, 10, 3600));
    config.memory = static_cast<uint64_t>(count("cache", "memory_limit_mb", 64, 0, 4096)) << 20;
    config.disk = static_cast<uint64_t>(count("cache", "disk_limit_gb", 10, 1, 4096)) << 30;
    config.prefetch = v("cache", "prefetch", true);
    config.write_back = v("cache", "write_back", true);
    config.encryption = v("security", "encryption", false);
    const auto level = v("logging", "level", std::string("error"));
    if (level != "error" && level != "info") throw std::runtime_error("logging.level must be error or info");
    config.info_log = level == "info";
    return config;
}

void log(const Config& c, std::string_view message, bool error) noexcept {
    if (!error && !c.info_log) return;
    try {
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        const auto path = c.root / L"telegramfs.log";
        std::error_code ec;
        if (fs::file_size(path, ec) > (4ull << 20) && !ec) {
            MoveFileExW(path.c_str(), (c.root / L"telegramfs.log.1").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        Handle out(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (out) {
            SYSTEMTIME time{};
            GetSystemTime(&time);
            char stamp[32];
            std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
            const auto line = std::string(stamp) + std::string(message) + "\r\n";
            DWORD written;
            WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
    } catch (...) {}
}

Bytes read_range(const fs::path& path, uint64_t offset, size_t length) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw WinError(GetLastError(), "Open local data");
    LARGE_INTEGER where{};
    where.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, where, nullptr, FILE_BEGIN)) throw WinError(GetLastError(), "Seek local data");
    Bytes data(length);
    size_t position = 0;
    while (position < length) {
        DWORD got = 0;
        const auto step = static_cast<DWORD>(std::min<size_t>(length - position, 1u << 24));
        if (!ReadFile(file, data.data() + position, step, &got, nullptr))
            throw WinError(GetLastError(), "Read local data");
        if (!got) throw WinError(ERROR_HANDLE_EOF, "Truncated local data");
        position += got;
    }
    return data;
}

Bytes read_file(const fs::path& path, uint64_t maximum) {
    const auto length = fs::file_size(path);
    if (length > maximum || length > SIZE_MAX) throw WinError(ERROR_FILE_TOO_LARGE, "Local object too large");
    return read_range(path, 0, static_cast<size_t>(length));
}

void erase_if_exists(const fs::path& path) noexcept {
    std::error_code ec;
    fs::remove(path, ec);
}

void atomic_write(const fs::path& path, std::span<const uint8_t> bytes) {
    auto temporary = path;
    temporary += L"." + wide(uuid()) + L".tmp";
    Finally remove{[&] { erase_if_exists(temporary); }};
    {
        Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) throw WinError(GetLastError(), "Create local transaction");
        size_t position = 0;
        while (position < bytes.size()) {
            DWORD written = 0;
            const auto step = static_cast<DWORD>(std::min<size_t>(bytes.size() - position, 1u << 24));
            if (!WriteFile(file, bytes.data() + position, step, &written, nullptr))
                throw WinError(GetLastError(), "Write local transaction");
            if (!written) throw WinError(ERROR_WRITE_FAULT, "Empty local write");
            position += written;
        }
        if (!FlushFileBuffers(file)) throw WinError(GetLastError(), "Flush local transaction");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw WinError(GetLastError(), "Commit local transaction");
}

void atomic_write(const fs::path& path, std::string_view text) { atomic_write(path, bytes_of(text)); }

uint64_t filetime() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

uint64_t from_unix_ms(int64_t ms) {
    constexpr int64_t epoch = 11644473600000ll;
    if (ms < -epoch || ms > 900000000000000ll) return 0;
    return static_cast<uint64_t>((ms + epoch) * 10000ll);
}

std::string uuid() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("CoCreateGuid failed");
    wchar_t text[40]{};
    if (!StringFromGUID2(id, text, 40)) throw std::runtime_error("StringFromGUID2 failed");
    return utf8(std::wstring_view(text + 1, 36));
}

std::span<const uint8_t> bytes_of(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
std::string string_of(std::span<const uint8_t> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
}
