#pragma once
#include "store.hpp"
#include <winternl.h>
using PNTSTATUS = NTSTATUS*;
#ifdef __MINGW32__
#undef _ReadWriteBarrier
#endif
#include <winfsp/winfsp.h>

namespace tfs {
class Volume {
public:
    explicit Volume(Store&);
    ~Volume();
    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;
    void mount();
    void stop();
    bool failed() const { return dispatcher_failed_.load(); }

private:
    struct OpenFile {
        Volume& owner;
        Entry node;
        std::mutex read_mutex;
        uint64_t previous_end = UINT64_MAX;
        unsigned streak = 0;
        explicit OpenFile(Volume& owner, Entry node);
        ~OpenFile();
    };

    static Volume& self(FSP_FILE_SYSTEM* f) { return *static_cast<Volume*>(f->UserContext); }
    static OpenFile& opened(PVOID file) { return *static_cast<OpenFile*>(file); }
    static void describe(const Node&, FSP_FSCTL_FILE_INFO*);
    static const FSP_FILE_SYSTEM_INTERFACE callbacks_;
    void normalize(const Entry&, FSP_FSCTL_FILE_INFO*);
    NTSTATUS security(const Node&, PSECURITY_DESCRIPTOR, SIZE_T*);
    template<class F> NTSTATUS call(F&& f) noexcept;
    template<class F> void cleanup(F&& f) noexcept;
    void check_access(const Entry&, UINT32 options, UINT32 access);

    Store& store_;
    FSP_FILE_SYSTEM* filesystem_ = nullptr;
    HMODULE module_ = nullptr;
    PSECURITY_DESCRIPTOR read_write_ = nullptr, read_only_ = nullptr;
    decltype(&FspFileSystemCreate) create_ = nullptr;
    decltype(&FspFileSystemDelete) destroy_ = nullptr;
    decltype(&FspFileSystemSetMountPoint) set_mount_ = nullptr;
    decltype(&FspFileSystemStartDispatcher) start_dispatcher_ = nullptr;
    decltype(&FspFileSystemStopDispatcher) stop_dispatcher_ = nullptr;
    decltype(&FspFileSystemAddDirInfo) add_dir_ = nullptr;
    decltype(&FspNtStatusFromWin32) nt_status_ = nullptr;
    std::atomic<bool> dispatcher_failed_{false};
};
}
