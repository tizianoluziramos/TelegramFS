#include "filesystem.hpp"
#include <sddl.h>
#include <cstring>

namespace tfs {
namespace {
constexpr auto write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
    FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
constexpr auto label = L"TelegramFS";

template<class T> T load(HMODULE module, const char* name) {
    auto pointer = GetProcAddress(module, name);
    if (!pointer) throw WinError(GetLastError(), std::string("WinFsp symbol: ") + name);
    return reinterpret_cast<T>(pointer);
}

uint64_t inode(std::string_view id) {
    uint64_t result = 14695981039346656037ull;
    for (char c : id) { result ^= static_cast<uint8_t>(c); result *= 1099511628211ull; }
    return result ? result : 1;
}

PSECURITY_DESCRIPTOR descriptor(const std::wstring& sid, bool read_only) {
    const auto permissions = read_only ? L"FRFX" : L"FA";
    const auto sddl = L"O:" + sid + L"G:" + sid + L"D:P(A;;FA;;;SY)(A;;" + permissions + L";;;" + sid + L")";
    PSECURITY_DESCRIPTOR result = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &result, nullptr))
        throw WinError(GetLastError(), "Create volume access descriptor");
    return result;
}
}

Volume::OpenFile::OpenFile(Volume& v, Entry entry) : owner(v), node(std::move(entry)) {
    if (!node->directory()) owner.store_.open_file();
}
Volume::OpenFile::~OpenFile() {
    if (!node->directory()) owner.store_.close_file();
}

Volume::Volume(Store& store) : store_(store) {
    wchar_t install[32768]{};
    DWORD count = sizeof(install);
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, L"" FSP_FSCTL_PRODUCT_FULL_REGKEY,
        L"InstallDir", RRF_RT_REG_SZ, nullptr, install, &count);
    if (status != ERROR_SUCCESS) throw WinError(status, "WinFsp installation registry entry");
    auto dll = fs::path(install) / L"bin" / L"winfsp-x64.dll";
    module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) throw WinError(GetLastError(), "Load WinFsp x64");
    try {
        create_ = load<decltype(create_)>(module_, "FspFileSystemCreate");
        destroy_ = load<decltype(destroy_)>(module_, "FspFileSystemDelete");
        set_mount_ = load<decltype(set_mount_)>(module_, "FspFileSystemSetMountPoint");
        start_dispatcher_ = load<decltype(start_dispatcher_)>(module_, "FspFileSystemStartDispatcher");
        stop_dispatcher_ = load<decltype(stop_dispatcher_)>(module_, "FspFileSystemStopDispatcher");
        add_dir_ = load<decltype(add_dir_)>(module_, "FspFileSystemAddDirInfo");
        nt_status_ = load<decltype(nt_status_)>(module_, "FspNtStatusFromWin32");
        const auto sid = user_sid();
        read_write_ = descriptor(sid, false);
        read_only_ = descriptor(sid, true);
    } catch (...) {
        if (read_write_) LocalFree(read_write_);
        FreeLibrary(module_);
        throw;
    }
}

Volume::~Volume() {
    stop();
    LocalFree(read_only_);
    LocalFree(read_write_);
    FreeLibrary(module_);
}

void Volume::mount() {
    FSP_FSCTL_VOLUME_PARAMS params{};
    params.Version = sizeof(params);
    params.SectorSize = 512;
    params.SectorsPerAllocationUnit = 8;
    params.MaxComponentLength = 255;
    params.VolumeCreationTime = store_.find(L"\\")->creation;
    params.VolumeSerialNumber = static_cast<uint32_t>(inode(std::to_string(store_.config().channel)));
    params.FileInfoTimeout = 250;
    params.CaseSensitiveSearch = 0;
    params.CasePreservedNames = 1;
    params.UnicodeOnDisk = 1;
    params.ReadOnlyVolume = store_.config().read_only;
    params.PostCleanupWhenModifiedOnly = 1;
    params.UmFileContextIsUserContext2 = 1;
    params.FlushAndPurgeOnCleanup = 1;
    const wchar_t fsname[] = L"-TelegramFS";
    std::copy(std::begin(fsname), std::end(fsname), params.FileSystemName);
    std::wstring device = L"" FSP_FSCTL_DISK_DEVICE_NAME;
    auto s = create_(device.data(), &params, &callbacks_, &filesystem_);
    if (s < 0) throw std::runtime_error("WinFsp volume creation failed. NTSTATUS " + std::to_string(uint32_t(s)));
    filesystem_->UserContext = this;
    auto name = store_.config().mount;
    s = set_mount_(filesystem_, name.data());
    if (s < 0) throw std::runtime_error("WinFsp drive letter mount failed. NTSTATUS " + std::to_string(uint32_t(s)));
    s = start_dispatcher_(filesystem_, 0);
    if (s < 0) throw std::runtime_error("WinFsp dispatcher failed. NTSTATUS " + std::to_string(uint32_t(s)));
}

void Volume::stop() {
    if (!filesystem_) return;
    stop_dispatcher_(filesystem_);
    destroy_(filesystem_);
    filesystem_ = nullptr;
}

template<class F> NTSTATUS Volume::call(F&& f) noexcept {
    try {
        return f();
    } catch (const WinError& e) {
        if (e.code == ERROR_FILE_CORRUPT || e.code == ERROR_CRC || e.code == ERROR_DISK_FULL ||
            e.code == ERROR_REVISION_MISMATCH)
            log(store_.config(), e.what());
        return nt_status_(e.code);
    } catch (const TdError& e) {
        log(store_.config(), e.what());
        if (e.code == 408 || e.code == 429) return STATUS_IO_TIMEOUT;
        if (e.code == 499) return STATUS_CANCELLED;
        if (e.code == 401 || e.code == 403) return STATUS_ACCESS_DENIED;
        if (e.code == 400 || e.code == 404) return STATUS_FILE_NOT_AVAILABLE;
        return STATUS_UNEXPECTED_NETWORK_ERROR;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    } catch (const std::exception& e) {
        log(store_.config(), e.what());
        return STATUS_IO_DEVICE_ERROR;
    } catch (...) {
        log(store_.config(), "Unexpected filesystem callback failure");
        return STATUS_INTERNAL_ERROR;
    }
}

template<class F> void Volume::cleanup(F&& f) noexcept {
    try { f(); }
    catch (const std::exception& e) { log(store_.config(), e.what()); }
    catch (...) { log(store_.config(), "Unexpected cleanup failure"); }
}

void Volume::describe(const Node& n, FSP_FSCTL_FILE_INFO* out) {
    *out = {};
    out->FileAttributes = n.attributes;
    out->FileSize = n.size;
    out->AllocationSize = n.directory() ? 0 : std::max(n.allocation, (n.size + 4095) & ~uint64_t(4095));
    out->CreationTime = n.creation;
    out->LastAccessTime = n.access;
    out->LastWriteTime = n.modified;
    out->ChangeTime = n.change;
    out->IndexNumber = inode(n.id);
}

void Volume::normalize(const Entry& node, FSP_FSCTL_FILE_INFO* info) {
    std::wstring full;
    auto entry = node;
    while (entry->id != root_id) {
        full.insert(0, L"\\" + entry->name);
        entry = store_.parent(entry);
    }
    if (full.empty()) full = L"\\";
    auto* open = FspFileSystemGetOpenFileInfo(info);
    size_t size = full.size() * sizeof(wchar_t);
    if (open->NormalizedName && size <= open->NormalizedNameSize) {
        std::memcpy(open->NormalizedName, full.data(), size);
        open->NormalizedNameSize = static_cast<UINT16>(size);
    }
}

NTSTATUS Volume::security(const Node& node, PSECURITY_DESCRIPTOR out, SIZE_T* len) {
    if (!len) return STATUS_SUCCESS;
    auto sd = (store_.config().read_only || node.legacy_chat || node.id == imports_id) ? read_only_ : read_write_;
    const auto actual = GetSecurityDescriptorLength(sd);
    if (*len < actual) {
        *len = actual;
        return STATUS_BUFFER_OVERFLOW;
    }
    *len = actual;
    if (out) std::memcpy(out, sd, actual);
    return STATUS_SUCCESS;
}

void Volume::check_access(const Entry& node, UINT32 options, UINT32 access) {
    if ((options & FILE_DIRECTORY_FILE) && !node->directory())
        throw WinError(ERROR_DIRECTORY, "Requested object is a file");
    if ((options & FILE_NON_DIRECTORY_FILE) && node->directory())
        throw WinError(ERROR_ACCESS_DENIED, "Requested object is a directory");
    if ((store_.config().read_only || node->legacy_chat || node->id == imports_id) && (access & write_rights))
        throw WinError(ERROR_WRITE_PROTECT, "Read-only object");
    if (options & FILE_DELETE_ON_CLOSE) store_.can_delete(node);
}

const FSP_FILE_SYSTEM_INTERFACE Volume::callbacks_ = [] {
    FSP_FILE_SYSTEM_INTERFACE c{};
    c.GetVolumeInfo = [](auto* f, auto* out) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            *out = {};
            out->TotalSize = v.store_.config().disk;
            out->FreeSize = v.store_.config().read_only ? 0 : v.store_.free_space();
            out->VolumeLabelLength = static_cast<UINT16>(std::wcslen(label) * sizeof(wchar_t));
            std::copy_n(label, std::wcslen(label), out->VolumeLabel);
            return STATUS_SUCCESS;
        });
    };
    c.GetSecurityByName = [](FSP_FILE_SYSTEM* f, PWSTR name, PUINT32 attrs,
        PSECURITY_DESCRIPTOR sd, SIZE_T* size) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            const auto node = v.store_.find(name);
            if (attrs) *attrs = node->attributes;
            return v.security(*node, sd, size);
        });
    };
    c.Create = [](FSP_FILE_SYSTEM* f, PWSTR name, UINT32 options, UINT32, UINT32 attrs,
        PSECURITY_DESCRIPTOR, UINT64 allocation, PVOID* context, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            if (options & FILE_DIRECTORY_FILE) attrs |= FILE_ATTRIBUTE_DIRECTORY;
            else attrs &= ~FILE_ATTRIBUTE_DIRECTORY;
            if ((options & FILE_DELETE_ON_CLOSE) && (attrs & FILE_ATTRIBUTE_READONLY))
                return STATUS_CANNOT_DELETE;
            auto file = std::make_unique<OpenFile>(v, v.store_.create(name, attrs, allocation));
            describe(*file->node, info);
            v.normalize(file->node, info);
            *context = file.release();
            return STATUS_SUCCESS;
        });
    };
    c.Open = [](FSP_FILE_SYSTEM* f, PWSTR name, UINT32 options, UINT32 access, PVOID* context,
        FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            auto node = v.store_.find(name);
            if ((options & FILE_DIRECTORY_FILE) && !node->directory()) return STATUS_NOT_A_DIRECTORY;
            if ((options & FILE_NON_DIRECTORY_FILE) && node->directory()) return STATUS_FILE_IS_A_DIRECTORY;
            v.check_access(node, options, access);
            auto file = std::make_unique<OpenFile>(v, node);
            describe(*node, info);
            v.normalize(node, info);
            *context = file.release();
            return STATUS_SUCCESS;
        });
    };
    c.Overwrite = [](FSP_FILE_SYSTEM* f, PVOID context, UINT32 attrs, BOOLEAN replace, UINT64 size,
        FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            auto n = v.store_.current(opened(context).node);
            if (!replace) attrs |= n->attributes;
            describe(*v.store_.resize(n, size, true, attrs), info);
            return STATUS_SUCCESS;
        });
    };
    c.Cleanup = [](FSP_FILE_SYSTEM* f, PVOID context, PWSTR, ULONG flags) {
        auto& v = self(f);
        v.cleanup([&] {
            const auto& node = opened(context).node;
            if (flags & FspCleanupDelete) v.store_.remove(node);
            if (flags) v.store_.wake();
        });
    };
    c.Close = [](FSP_FILE_SYSTEM*, PVOID context) { delete static_cast<OpenFile*>(context); };
    c.Read = [](FSP_FILE_SYSTEM* f, PVOID context, PVOID buffer, UINT64 offset, ULONG size,
        PULONG transferred) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            *transferred = 0;
            if (!size) return STATUS_SUCCESS;
            auto& handle = opened(context);
            auto node = v.store_.current(handle.node);
            auto length = v.store_.read(node, offset, {static_cast<uint8_t*>(buffer), size});
            *transferred = static_cast<ULONG>(length);
            if (!length) return STATUS_END_OF_FILE;
            if (v.store_.config().prefetch) {
                std::lock_guard lock(handle.read_mutex);
                handle.streak = offset == handle.previous_end ? std::min(16u, handle.streak + 1) : 0;
                handle.previous_end = offset + length;
                if (handle.streak >= 2) v.store_.prefetch(node, handle.previous_end, std::min(4u, handle.streak / 2));
            }
            return STATUS_SUCCESS;
        });
    };
    c.Write = [](FSP_FILE_SYSTEM* f, PVOID context, PVOID buffer, UINT64 offset, ULONG size, BOOLEAN append,
        BOOLEAN constrained, PULONG transferred, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            *transferred = 0;
            size_t count = 0;
            auto entry = v.store_.write(opened(context).node, offset, {static_cast<const uint8_t*>(buffer), size},
                append != 0 && !constrained, constrained != 0, count);
            *transferred = static_cast<ULONG>(count);
            describe(*entry, info);
            return STATUS_SUCCESS;
        });
    };
    c.Flush = [](FSP_FILE_SYSTEM* f, PVOID context, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            if (context) describe(*v.store_.current(opened(context).node), info);
            if (!v.store_.config().read_only) {
                if (!v.store_.config().write_back && !v.store_.flush(true)) return STATUS_IO_TIMEOUT;
                v.store_.wake();
            }
            return STATUS_SUCCESS;
        });
    };
    c.GetFileInfo = [](FSP_FILE_SYSTEM* f, PVOID context, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            describe(*v.store_.current(opened(context).node), info);
            return STATUS_SUCCESS;
        });
    };
    c.SetBasicInfo = [](FSP_FILE_SYSTEM* f, PVOID context, UINT32 attrs, UINT64 create, UINT64 access,
        UINT64 write, UINT64 change, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            describe(*v.store_.set_basic(opened(context).node, attrs, create, access, write, change), info);
            return STATUS_SUCCESS;
        });
    };
    c.SetFileSize = [](FSP_FILE_SYSTEM* f, PVOID context, UINT64 size, BOOLEAN allocation,
        FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            describe(*v.store_.resize(opened(context).node, size, allocation != 0), info);
            return STATUS_SUCCESS;
        });
    };
    c.CanDelete = [](FSP_FILE_SYSTEM* f, PVOID context, PWSTR) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            v.store_.can_delete(opened(context).node);
            return STATUS_SUCCESS;
        });
    };
    c.Rename = [](FSP_FILE_SYSTEM* f, PVOID context, PWSTR, PWSTR name, BOOLEAN replace) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            v.store_.rename(opened(context).node, name, replace != 0);
            return STATUS_SUCCESS;
        });
    };
    c.GetSecurity = [](FSP_FILE_SYSTEM* f, PVOID context, PSECURITY_DESCRIPTOR sd, SIZE_T* len) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] { return v.security(*v.store_.current(opened(context).node), sd, len); });
    };
    c.SetSecurity = [](FSP_FILE_SYSTEM*, PVOID, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR) -> NTSTATUS {
        return STATUS_NOT_SUPPORTED;
    };
    c.ReadDirectory = [](FSP_FILE_SYSTEM* f, PVOID context, PWSTR, PWSTR marker, PVOID buffer, ULONG size,
        PULONG count) -> NTSTATUS {
        auto& v = self(f);
        return v.call([&] {
            *count = 0;
            const auto node = v.store_.current(opened(context).node);
            alignas(FSP_FSCTL_DIR_INFO) std::array<uint8_t, sizeof(FSP_FSCTL_DIR_INFO) + 510> data{};
            auto* dir = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(data.data());
            const auto add = [&](const Node& item, std::wstring_view name) {
                const auto bytes = name.size() * sizeof(wchar_t);
                dir->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + bytes);
                describe(item, &dir->FileInfo);
                std::memcpy(dir->FileNameBuf, name.data(), bytes);
                return v.add_dir_(dir, buffer, size, count) != 0;
            };
            std::wstring_view after = marker ? marker : L"";
            if (node->id != root_id) {
                if (after.empty() && !add(*node, L".")) return STATUS_SUCCESS;
                if ((after.empty() || after == L".") && !add(*v.store_.parent(node), L".."))
                    return STATUS_SUCCESS;
                if (after == L"." || after == L"..") after = {};
            }
            const auto maximum = (size - *count) / sizeof(FSP_FSCTL_DIR_INFO) + 1;
            const auto entries = v.store_.list(node, after, maximum);
            for (const auto& entry : entries) if (!add(*entry, entry->name)) return STATUS_SUCCESS;
            v.add_dir_(nullptr, buffer, size, count);
            return STATUS_SUCCESS;
        });
    };
    c.DispatcherStopped = [](FSP_FILE_SYSTEM* f, BOOLEAN normally) {
        if (!normally) self(f).dispatcher_failed_ = true;
    };
    return c;
}();
}
