#include "pomodoro/session_id.h"

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <cstdlib>
#else
#error A secure operating-system entropy source is required for session IDs.
#endif

namespace pomodoro {

SessionEntropy secureSessionEntropy() {
    SessionEntropy bytes{};
#ifdef _WIN32
    // Resolve from System32 explicitly so adding secure IDs cannot introduce an
    // implicit bcrypt.dll search beside the downloaded executable.
    class RandomLibrary final {
    public:
        using Generate = NTSTATUS (WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
        RandomLibrary() {
            module_ = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module_) throw std::runtime_error("Windows secure random generator is unavailable.");
            const auto address = GetProcAddress(module_, "BCryptGenRandom");
            static_assert(sizeof(address) == sizeof(generate));
            std::memcpy(&generate, &address, sizeof(generate));
            if (!generate) {
                FreeLibrary(module_);
                module_ = nullptr;
                throw std::runtime_error("Windows secure random generator is unavailable.");
            }
        }
        ~RandomLibrary() { if (module_) FreeLibrary(module_); }
        RandomLibrary(const RandomLibrary&) = delete;
        RandomLibrary& operator=(const RandomLibrary&) = delete;
        Generate generate = nullptr;
    private:
        HMODULE module_ = nullptr;
    };
    static const RandomLibrary library;
    const auto status = library.generate(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::runtime_error("Windows secure random generator failed.");
#elif defined(__linux__)
    std::size_t obtained = 0;
    while (obtained < bytes.size()) {
        const auto count = getrandom(bytes.data() + obtained, bytes.size() - obtained, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Operating-system secure random generator failed.");
        obtained += static_cast<std::size_t>(count);
    }
#else
    arc4random_buf(bytes.data(), bytes.size());
#endif
    return bytes;
}

std::string encodeSessionId(const SessionEntropy& entropy) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(entropy.size() * 2);
    for (const auto byte : entropy) {
        result += digits[byte >> 4];
        result += digits[byte & 0x0f];
    }
    return result;
}

} // namespace pomodoro
