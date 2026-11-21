#include "crypto.hpp"
#include <bcrypt.h>
#include <wincrypt.h>
#include <cstring>

namespace tfs {
namespace {
void check(NTSTATUS value, const char* operation) {
    if (value < 0) throw WinError(ERROR_INVALID_DATA, operation);
}
PUCHAR input(std::span<const uint8_t> value) { return const_cast<PUCHAR>(value.data()); }
ULONG narrow(size_t n) {
    if (n > ULONG_MAX) throw WinError(ERROR_FILE_TOO_LARGE, "Cryptographic buffer is too large");
    return static_cast<ULONG>(n);
}
struct Algorithm {
    BCRYPT_ALG_HANDLE value = nullptr;
    Algorithm(LPCWSTR name, ULONG flags = 0) {
        check(BCryptOpenAlgorithmProvider(&value, name, nullptr, flags), "Open CNG provider");
    }
    ~Algorithm() { BCryptCloseAlgorithmProvider(value, 0); }
};
BCRYPT_ALG_HANDLE aes_algorithm() {
    static Algorithm algorithm(BCRYPT_AES_ALGORITHM);
    static const bool ready = [&] {
        check(BCryptSetProperty(algorithm.value, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0), "Set AES-GCM");
        return true;
    }();
    (void)ready;
    return algorithm.value;
}

Bytes crypt(bool decrypt, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
    std::span<const uint8_t> aad, std::span<const uint8_t> text, std::span<uint8_t> tag) {
    if (key.size() != 32 || nonce.size() != 12 || tag.size() != 16)
        throw WinError(ERROR_INVALID_DATA, "Invalid AES-GCM parameters");
    BCRYPT_KEY_HANDLE handle = nullptr;
    check(BCryptGenerateSymmetricKey(aes_algorithm(), &handle, nullptr, 0, input(key), 32, 0),
        "Generate AES key");
    Finally release{[&] { BCryptDestroyKey(handle); }};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = input(nonce);
    info.cbNonce = 12;
    info.pbAuthData = input(aad);
    info.cbAuthData = narrow(aad.size());
    info.pbTag = tag.data();
    info.cbTag = 16;
    Bytes output(std::max<size_t>(text.size(), 1));
    ULONG count = 0;
    auto fn = decrypt ? BCryptDecrypt : BCryptEncrypt;
    check(fn(handle, input(text), narrow(text.size()), &info, nullptr, 0, output.data(),
        narrow(output.size()), &count, 0), decrypt ? "AES-GCM authentication failed" : "AES-GCM encryption failed");
    output.resize(count);
    return output;
}

Secret pbkdf(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations) {
    if (password.empty()) throw std::runtime_error("An empty recovery password is invalid");
    static Algorithm sha(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Secret result(Bytes(32));
    check(BCryptDeriveKeyPBKDF2(sha.value, input(bytes_of(password)), narrow(password.size()),
        input(salt), narrow(salt.size()), iterations, result.bytes.data(), 32, 0), "Derive backup key");
    return result;
}

std::array<uint8_t, 4> be(uint32_t value) {
    return {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}
}

Bytes random_bytes(size_t size) {
    Bytes value(size);
    check(BCryptGenRandom(nullptr, value.data(), narrow(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "CNG random");
    return value;
}

Bytes dpapi(std::span<const uint8_t> value, bool decrypt) {
    DATA_BLOB in{narrow(value.size()), input(value)}, out{};
    const auto ok = decrypt
        ? CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
        : CryptProtectData(&in, L"TelegramFS", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) throw WinError(GetLastError(), "Windows DPAPI");
    Finally release{[&] {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }};
    return {out.pbData, out.pbData + out.cbData};
}

Secret load_secret(const fs::path& path) { return Secret(dpapi(read_file(path, 65536), true)); }
void save_secret(const fs::path& path, std::span<const uint8_t> value) {
    atomic_write(path, dpapi(value, false));
}

std::string encode_base64(std::span<const uint8_t> data) {
    DWORD size = 0;
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data.data(), narrow(data.size()), flags, nullptr, &size))
        throw WinError(GetLastError(), "Base64 size");
    std::string encoded(size, '\0');
    if (!CryptBinaryToStringA(data.data(), narrow(data.size()), flags, encoded.data(), &size))
        throw WinError(GetLastError(), "Base64 encode");
    encoded.resize(size);
    return encoded;
}

Bytes decode_base64(std::string_view text) {
    DWORD size = 0;
    const auto length = narrow(text.size());
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    if (!CryptStringToBinaryA(text.data(), length, flags, nullptr, &size, nullptr, nullptr))
        throw WinError(ERROR_INVALID_DATA, "Invalid base64");
    Bytes data(size);
    if (!CryptStringToBinaryA(text.data(), length, flags, data.data(), &size, nullptr, nullptr))
        throw WinError(ERROR_INVALID_DATA, "Invalid base64");
    return data;
}

std::string digest(std::span<const uint8_t> data, std::span<const uint8_t> key) {
    static Algorithm sha(BCRYPT_SHA256_ALGORITHM);
    static Algorithm hmac(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptCreateHash(key.empty() ? sha.value : hmac.value, &hash, nullptr, 0,
        input(key), narrow(key.size()), 0), "Create SHA-256 hash");
    Finally release{[&] { BCryptDestroyHash(hash); }};
    check(BCryptHashData(hash, input(data), narrow(data.size()), 0), "Hash block");
    std::array<uint8_t, 32> result{};
    check(BCryptFinishHash(hash, result.data(), 32, 0), "Finalize hash");
    std::string hex;
    hex.reserve(64);
    for (auto byte : result) {
        constexpr char digits[] = "0123456789abcdef";
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}

bool is_digest(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

Bytes seal(std::span<const uint8_t> key, std::span<const uint8_t> plain, std::string_view aad) {
    auto nonce = random_bytes(12);
    std::array<uint8_t, 16> tag{};
    auto ciphertext = crypt(false, key, nonce, bytes_of(aad), plain, tag);
    nonce.insert(nonce.end(), ciphertext.begin(), ciphertext.end());
    nonce.insert(nonce.end(), tag.begin(), tag.end());
    return nonce;
}

Bytes unseal(std::span<const uint8_t> key, std::span<const uint8_t> sealed, std::string_view aad) {
    if (sealed.size() < 28) throw WinError(ERROR_INVALID_DATA, "Truncated encrypted object");
    std::array<uint8_t, 16> tag{};
    std::copy(sealed.end() - 16, sealed.end(), tag.begin());
    return crypt(true, key, sealed.first(12), bytes_of(aad), sealed.subspan(12, sealed.size() - 28), tag);
}

uint32_t big_endian(std::span<const uint8_t> data) {
    if (data.size() < 4) throw WinError(ERROR_INVALID_DATA, "Truncated big-endian integer");
    return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3];
}

Bytes legacy_frame(std::span<const uint8_t> key, std::span<const uint8_t> header,
    uint32_t frame, std::span<const uint8_t> stored, bool last) {
    if (header.size() != 13 || string_of(header.first(4)) != "TDE1" || header[4] != 1)
        throw WinError(ERROR_INVALID_DATA, "Invalid DriveInfinity encrypted file header");
    uint32_t length = big_endian(stored);
    if (bool(length >> 31) != last || (length & 0x7fffffff) != stored.size() - 4 || stored.size() < 20)
        throw WinError(ERROR_INVALID_DATA, "Invalid DriveInfinity encrypted frame header");
    const auto index = be(frame);
    std::array<uint8_t, 12> nonce{};
    std::copy(header.begin() + 5, header.end(), nonce.begin());
    std::copy(index.begin(), index.end(), nonce.begin() + 8);
    const std::array<uint8_t, 9> aad{0, 0, 0, 1, index[0], index[1], index[2], index[3], uint8_t(last)};
    std::array<uint8_t, 16> tag{};
    std::copy(stored.end() - 16, stored.end(), tag.begin());
    return crypt(true, key, nonce, aad, stored.subspan(4, stored.size() - 20), tag);
}

Bytes make_key_backup(std::span<const uint8_t> key, std::string_view password) {
    constexpr uint32_t iterations = 310000;
    const auto salt = random_bytes(16);
    const auto wrapping = pbkdf(password, salt, iterations);
    const auto sealed = seal(wrapping.bytes, key);
    const auto count = be(iterations);
    Bytes result{'T', 'D', 'K', 'B', 1};
    result.insert(result.end(), count.begin(), count.end());
    result.insert(result.end(), salt.begin(), salt.end());
    result.insert(result.end(), 2, 0);
    result.insert(result.end(), sealed.begin(), sealed.end());
    return result;
}

Secret restore_key_backup(std::span<const uint8_t> backup, std::string_view password) {
    if (backup.size() < 87 || string_of(backup.first(4)) != "TDKB" || backup[4] != 1)
        throw WinError(ERROR_INVALID_DATA, "Invalid recovery document");
    const auto iterations = big_endian(backup.subspan(5, 4));
    const uint16_t hint = static_cast<uint16_t>((uint16_t(backup[25]) << 8) | backup[26]);
    if (iterations < 10000 || iterations > 10000000 || hint > 512 || backup.size() != 87ull + hint)
        throw WinError(ERROR_INVALID_DATA, "Invalid recovery document parameters");
    const auto wrapping = pbkdf(password, backup.subspan(9, 16), iterations);
    return Secret(unseal(wrapping.bytes, backup.subspan(27 + hint)));
}
}
