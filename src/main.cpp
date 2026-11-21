#include "filesystem.hpp"
#include <charconv>
#include <iostream>

namespace tfs {
using namespace std::chrono_literals;
namespace {
Handle quit_signal;
Handle sync_signal;
constexpr auto run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring object_name(std::wstring_view suffix) {
    return L"Global\\TelegramFS-" + user_sid() + L"-" + std::wstring(suffix);
}

class Exclusive {
public:
    explicit Exclusive(bool required = true)
        : handle_(CreateMutexW(nullptr, FALSE, object_name(L"session").c_str())) {
        if (!handle_) throw WinError(GetLastError(), "Open session mutex");
        const auto result = WaitForSingleObject(handle_, 0);
        held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if (result == WAIT_FAILED) throw WinError(GetLastError(), "Check session mutex");
        if (!held_ && required) throw std::runtime_error("TelegramFS already owns the Telegram session");
    }
    ~Exclusive() { if (held_) ReleaseMutex(handle_); }
    bool held() const { return held_; }
private:
    Handle handle_;
    bool held_ = false;
};

Handle named_event(std::wstring_view suffix) {
    Handle result(CreateEventW(nullptr, TRUE, FALSE, object_name(suffix).c_str()));
    if (!result) throw WinError(GetLastError(), "Create control event");
    ResetEvent(result);
    return result;
}

std::wstring quote(std::wstring_view arg) {
    std::wstring output = L"\"";
    size_t slashes = 0;
    for (auto c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        if (c == L'\"') output += L'\\';
        output += c;
        slashes = 0;
    }
    output.append(slashes * 2, L'\\');
    output += L'\"';
    return output;
}

BOOL WINAPI console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (quit_signal) SetEvent(quit_signal);
        return TRUE;
    }
    return FALSE;
}

void status_file(const Config& config, Json value) {
    value["drive"] = utf8(config.mount);
    value["pid"] = GetCurrentProcessId();
    value["channel_id"] = config.channel;
    atomic_write(config.root / L"status.json", value.dump());
}

Json read_status(const Config& config) {
    const auto path = config.root / L"status.json";
    return fs::exists(path) ? Json::parse(read_file(path, 65536)) : Json::object();
}

void bind_account(const Config& config, Telegram& telegram) {
    const auto id = telegram.request(obj("getMe")).at("id").get<int64_t>();
    if (id <= 0) throw std::runtime_error("Telegram account is unavailable");
    const auto text = std::to_string(id);
    const auto path = config.root / L"account.dpapi";
    if (fs::exists(path)) {
        const auto account = load_secret(path);
        if (string_of(account.bytes) != text)
            throw std::runtime_error("This local profile belongs to a different Telegram account");
    } else {
        save_secret(path, bytes_of(text));
    }
}

int mounted(Config config) {
    Exclusive lock;
    quit_signal = named_event(L"stop");
    sync_signal = named_event(L"sync");
    SetConsoleCtrlHandler(console_event, TRUE);
    Finally release{[] {
        SetConsoleCtrlHandler(console_event, FALSE);
        if (quit_signal) CloseHandle(std::exchange(quit_signal.value, nullptr));
        if (sync_signal) CloseHandle(std::exchange(sync_signal.value, nullptr));
    }};
    try {
        status_file(config, {{"state","starting"}});
        if (config.channel >= 0)
            throw std::runtime_error("Set telegram.channel_id in the configuration. Use channels or channel-create");
        Telegram telegram(config);
        telegram.authenticate(false);
        bind_account(config, telegram);
        telegram.verify_channel(config.channel, !config.read_only);
        Store store(config, telegram);
        Volume volume(store);
        store.start();
        volume.mount();
        Json previous;
        auto status = [&] {
            Json state = {{"state","mounted"},{"connected",telegram.connected()},
                {"pending_local_changes",store.dirty()},{"flush",config.write_back ? "local" : "remote"}};
            if (state != previous) {
                status_file(config, state);
                previous = std::move(state);
            }
        };
        status();
        log(config, "Volume mounted", false);
        const HANDLE events[]{quit_signal, sync_signal};
        while (!volume.failed()) {
            const auto event = WaitForMultipleObjects(2, events, FALSE, 2000);
            if (event == WAIT_OBJECT_0) break;
            if (event == WAIT_FAILED) throw WinError(GetLastError(), "Wait for volume controls");
            if (event == WAIT_OBJECT_0 + 1) {
                ResetEvent(sync_signal);
                store.request_sync();
            }
            try { status(); }
            catch (const std::exception& e) { log(config, e.what()); }
        }
        store.stop();
        volume.stop();
        status_file(config, {{"state","unmounted"},{"pending_local_changes",store.dirty()}});
        log(config, "Volume unmounted", false);
        if (volume.failed()) throw std::runtime_error("WinFsp dispatcher stopped unexpectedly");
        return 0;
    } catch (const std::exception& e) {
        log(config, e.what());
        try { status_file(config, {{"state","error"},{"message",e.what()}}); } catch (...) {}
        return 1;
    }
}

int launch(const Config& config) {
    {
        Exclusive state(false);
        if (!state.held()) throw std::runtime_error("TelegramFS is already running");
    }
    auto executable = exe_path();
    auto command = quote(executable.wstring()) + L" __mount " + quote(config.mount);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        throw WinError(GetLastError(), "Start background filesystem");
    Handle process(info.hProcess), thread(info.hThread);
    const auto deadline = Clock::now() + 95s;
    while (Clock::now() < deadline) {
        if (WaitForSingleObject(process, 200) == WAIT_OBJECT_0) {
            const auto state = read_status(config);
            const auto error = state.value("message", std::string("Background process exited"));
            throw std::runtime_error(error);
        }
        const auto state = read_status(config);
        if (state.value("pid", DWORD{0}) == info.dwProcessId &&
            state.value("state", std::string{}) == "mounted") {
            std::wcout << L"Mounted " << config.mount << L'\n';
            return 0;
        }
    }
    throw std::runtime_error("Mount is still starting. Inspect telegramfs status");
}

void signal(const Config& config, std::wstring_view event, bool wait, bool check_drive) {
    Exclusive state(false);
    if (state.held()) throw std::runtime_error("TelegramFS is not mounted");
    if (check_drive) {
        const auto mount = read_status(config).value("drive", std::string{});
        if (mount != utf8(config.mount)) throw std::runtime_error("The requested drive letter is not mounted by TelegramFS");
    }
    Handle target(OpenEventW(EVENT_MODIFY_STATE, FALSE, object_name(event).c_str()));
    if (!target || !SetEvent(target)) throw WinError(GetLastError(), "Signal filesystem process");
    if (!wait) return;
    Handle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, object_name(L"session").c_str()));
    if (mutex) {
        const auto result = WaitForSingleObject(mutex, 60000);
        if (result == WAIT_TIMEOUT) throw std::runtime_error("Dismount is still in progress");
        if (result == WAIT_FAILED) throw WinError(GetLastError(), "Wait for filesystem process");
        ReleaseMutex(mutex);
    }
    const auto value = read_status(config);
    std::cout << "Unmounted";
    if (value.value("pending_local_changes", false)) std::cout << ". Local writes are queued for the next mount";
    std::cout << "\n";
}

void startup(const Config& config, bool on) {
    HKEY key = nullptr;
    const auto rc = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) throw WinError(rc, "Open per-user startup registry");
    Finally close{[&] { RegCloseKey(key); }};
    auto script = config.root / L"start-telegramfs.vbs";
    if (!on) {
        const auto del = RegDeleteValueW(key, L"TelegramFS");
        if (del != ERROR_SUCCESS && del != ERROR_FILE_NOT_FOUND) throw WinError(del, "Remove startup registration");
        erase_if_exists(script);
        return;
    }
    std::wstring command = quote(exe_path().wstring()) + L" __auto";
    std::wstring escaped;
    for (auto c : command) {
        escaped += c;
        if (c == L'\"') escaped += c;
    }
    auto text = L"\xFEFF" + std::wstring(L"CreateObject(\"WScript.Shell\").Run \"") + escaped + L"\", 0, False\r\n";
    atomic_write(script, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
        text.size() * sizeof(wchar_t)));
    std::wstring system(32768, L'\0');
    auto count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!count || count >= system.size()) throw WinError(GetLastError(), "Locate Windows Script Host");
    system.resize(count);
    const auto value = quote((fs::path(system) / L"wscript.exe").wstring()) + L" //B " + quote(script.wstring());
    const auto set = RegSetValueExW(key, L"TelegramFS", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (set != ERROR_SUCCESS) throw WinError(set, "Register startup command");
}

int64_t chat_id(std::wstring_view value) {
    const auto input = utf8(value);
    int64_t id{};
    auto result = std::from_chars(input.data(), input.data() + input.size(), id);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || id >= 0)
        throw std::runtime_error("Expected a negative Telegram channel ID");
    return id;
}

std::optional<Doc> key_document(Telegram& tg, int64_t channel, bool legacy) {
    std::optional<Doc> result;
    tg.search(channel, legacy ? "keybackup" : "telegramfskey", [&](const Doc& doc) {
        if (legacy ? !(doc.caption.starts_with(legacy_key_tag) || doc.name == "driveinfinity.keybackup")
            : doc.caption != key_tag || doc.name != "telegramfskey.bin") return true;
        result = doc;
        return false;
    });
    if (legacy && !result) {
        tg.search(channel, "", [&](const Doc& doc) {
            if (!doc.caption.starts_with(legacy_key_tag) && doc.name != "driveinfinity.keybackup") return true;
            result = doc;
            return false;
        });
    }
    return result;
}

Secret phrase(std::wstring_view prompt) {
    auto pass = read_password(prompt);
    Finally wipe{[&] { SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t)); }};
    auto utf = utf8(pass);
    Finally clear_utf{[&] { SecureZeroMemory(utf.data(), utf.size()); }};
    return Secret(Bytes(utf.begin(), utf.end()));
}

std::string_view as_text(const Secret& s) {
    if (s.bytes.empty()) return {};
    return {reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size()};
}

void key_command(Config& config, Telegram& tg, bool restore, std::optional<int64_t> legacy) {
    const int64_t id = legacy.value_or(config.channel);
    tg.verify_channel(id, !restore);
    const auto existing = key_document(tg, id, legacy.has_value());
    const auto path = legacy ? legacy_key_path(config, id) : native_key_path(config);
    if (restore) {
        if (!existing || existing->size > 4096)
            throw std::runtime_error("No valid recovery document found");
        auto blob = tg.download(*existing, 0, static_cast<size_t>(existing->size));
        const auto password = phrase(L"Recovery password: ");
        const auto key = restore_key_backup(blob, as_text(password));
        if (fs::exists(path) && load_secret(path).bytes != key.bytes)
            throw std::runtime_error("Recovery key differs from the protected local key");
        save_secret(path, key.bytes);
        std::cout << "Recovery key protected with Windows DPAPI\n";
        return;
    }
    if (existing && !fs::exists(path)) throw std::runtime_error("A recovery document already exists. Run key-restore");
    if (!fs::exists(path)) {
        bool has_index = false;
        tg.search(id, "telegramfsindex", [&](const Doc& doc) {
            if (!doc.caption.starts_with(index_tag)) return true;
            has_index = true;
            return false;
        });
        if (has_index) throw std::runtime_error("Use a new channel to enable encryption for a new volume");
    }
    const auto password = phrase(L"New recovery password: ");
    const auto repeated = phrase(L"Repeat recovery password: ");
    if (password.bytes != repeated.bytes) throw std::runtime_error("Recovery passwords differ");
    const auto key = fs::exists(path) ? load_secret(path) : Secret(random_bytes(32));
    const auto payload = make_key_backup(key.bytes, as_text(password));
    save_secret(path, key.bytes);
    const auto directory = config.root / wide(uuid());
    fs::create_directory(directory);
    const auto file = directory / L"telegramfskey.bin";
    Finally clean{[&] { erase_if_exists(file); erase_if_exists(directory); }};
    atomic_write(file, payload);
    const auto doc = tg.send_document(id, file, key_tag);
    erase_if_exists(file);
    if (doc.size != payload.size() || tg.download(doc, 0, payload.size()) != payload)
        throw std::runtime_error("Recovery document verification failed");
    std::cout << "Recovery document uploaded and verified\n";
}

void help() {
    std::cout <<
        "telegramfs login | logout | channels | channel-create <name>\n"
        "telegramfs mount [T:] | unmount [T:] | status | sync\n"
        "telegramfs key-init | key-restore [--legacy <channel-id>]\n"
        "telegramfs startup enable | startup disable | config\n"
        "With the volume unmounted:\n"
        "telegramfs mark <volume-path> favorite|unfavorite|archive|unarchive\n"
        "telegramfs marked favorite|archive | restore <volume-trash-path>\n"
        "Flush commits to local disk with write_back=true, and to Telegram with write_back=false.\n"
        "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\n"
        "https://github.com/winfsp/winfsp\n";
}

int run(int argc, wchar_t** argv) {
    const std::wstring command = argc > 1 ? argv[1] : L"help";
    if (command == L"help" || command == L"--help" || command == L"-h") { help(); return 0; }
    auto config = load_config();
    if (command == L"config" && argc == 2) {
        std::wcout << (config.root / L"telegramfs.toml").wstring() << L'\n';
        return 0;
    }
    if ((command == L"mount" || command == L"unmount" || command == L"__mount") && argc <= 3) {
        if (argc == 3) config.mount = mount_point(argv[2]);
        if (command == L"mount") return launch(config);
        if (command == L"__mount") return mounted(std::move(config));
        signal(config, L"stop", true, argc == 3);
        return 0;
    }
    if (command == L"__auto" && argc == 2) return config.automatic ? mounted(std::move(config)) : 0;
    if (command == L"version" && argc == 2) {
        std::cout << "TelegramFS 0.1.0 (TDLib 1.8.70, WinFsp 2.1)\n";
        return 0;
    }
    if (command == L"status" && argc == 2) {
        Exclusive state(false);
        auto value = read_status(config);
        if (state.held() && value.value("state", std::string{}) != "error") value["state"] = "unmounted";
        std::cout << value.dump(2) << '\n';
        return 0;
    }
    if (command == L"startup" && argc == 3) {
        const std::wstring action = argv[2];
        if (action != L"enable" && action != L"disable") throw std::runtime_error("Expected startup enable or disable");
        startup(config, action == L"enable");
        if (action == L"enable" && !config.automatic)
            std::cout << "Startup is registered. Set filesystem.auto_mount = true to activate it\n";
        return 0;
    }
    if (command == L"sync" && argc == 2) {
        bool running = false;
        { Exclusive lock(false); running = !lock.held(); }
        if (running) { signal(config, L"sync", false, false); std::cout << "Sync requested\n"; return 0; }
    }
    const bool catalog = (command == L"restore" && argc == 3) ||
        (command == L"marked" && argc == 3) || (command == L"mark" && argc == 4);
    const bool valid = catalog || ((command == L"login" || command == L"logout" || command == L"channels" ||
        command == L"key-init" || command == L"sync") && argc == 2) ||
        (command == L"channel-create" && argc == 3) ||
        (command == L"key-restore" && (argc == 2 || (argc == 4 && std::wstring_view(argv[2]) == L"--legacy")));
    if (!valid) { help(); return 2; }
    Exclusive state;
    Telegram telegram(config);
    telegram.authenticate(command == L"login");
    bind_account(config, telegram);
    if (command == L"login") std::cout << "Telegram session authenticated\n";
    else if (command == L"logout") { telegram.logout(); std::cout << "Telegram session closed\n"; }
    else if (command == L"channels") {
        for (const auto& chat : telegram.channels())
            std::wcout << chat.at("id").get<int64_t>() << L'\t' << wide(chat.at("title").get<std::string>()) << L'\n';
    } else if (command == L"channel-create") {
        std::cout << telegram.create_channel(utf8(argv[2])) << '\n';
    } else if (command == L"key-init" || command == L"key-restore") {
        const auto legacy = argc == 4 ? std::optional<int64_t>(chat_id(argv[3])) : std::nullopt;
        key_command(config, telegram, command == L"key-restore", legacy);
    } else if (catalog) {
        const auto action = command == L"mark" ? std::wstring_view(argv[3]) :
            command == L"marked" ? std::wstring_view(argv[2]) : std::wstring_view{};
        const bool favorite = action == L"favorite" || action == L"unfavorite";
        if (command == L"mark" && !favorite && action != L"archive" && action != L"unarchive")
            throw std::runtime_error("Expected favorite, unfavorite, archive or unarchive");
        if (command == L"marked" && action != L"favorite" && action != L"archive")
            throw std::runtime_error("Expected favorite or archive");
        telegram.verify_channel(config.channel, command != L"marked");
        Store store(config, telegram);
        store.refresh_now();
        if (command == L"marked") {
            for (const auto& path : store.marked(favorite)) std::wcout << path << L'\n';
        } else {
            std::wstring path = argv[2];
            if (path.size() >= 2 && path[1] == L':') path.erase(0, 2);
            if (command == L"restore") store.restore(path);
            else store.mark(path, favorite, action == L"favorite" || action == L"archive");
            if (!store.flush(true)) throw std::runtime_error("The operation is stored locally and remains queued");
        }
    } else if (command == L"sync") {
        telegram.verify_channel(config.channel, !config.read_only);
        Store store(config, telegram);
        store.refresh_now();
        if (store.dirty()) throw std::runtime_error("Local changes are still pending");
        std::cout << "Sync finished\n";
    }
    return 0;
}
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        return tfs::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "telegramfs: " << e.what() << '\n';
        return 1;
    }
}
