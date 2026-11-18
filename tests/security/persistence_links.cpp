// POSIX-only filesystem attack simulation. It neither accesses a real profile
// nor establishes Windows reparse-point behavior. See README.md for commands.
#ifdef _WIN32
#error This harness requires POSIX symlink and permission semantics.
#endif

// Include intentionally: raw persistence helpers are private implementation.
#include "../../src/persistence/state_store.cpp"

#include <cstdlib>
#include <iostream>
#include <sys/stat.h>

namespace {
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto pattern = (std::filesystem::temp_directory_path() /
            "pomodoro-security-links-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto created = mkdtemp(writable.data());
        if (!created) throw std::runtime_error("could not create isolated test directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};
} // namespace

int main() {
    using namespace pomodoro;
    try {
        TemporaryDirectory temporary;
        const auto canary = temporary.path / "canary.txt";
        constexpr char content[] = "unrelated-file-must-survive";
        {
            std::ofstream output(canary);
            output << content;
            if (!output) throw std::runtime_error("could not write canary");
        }
        for (int variant = 0; variant < 3; ++variant) {
            const auto directory = temporary.path / std::to_string(variant);
            std::filesystem::create_directory(directory);
            if (variant == 0)
                std::filesystem::create_symlink(canary, directory / "settings.json");
            if (variant == 1)
                std::filesystem::create_hard_link(canary, directory / "settings.json");
            if (variant == 2)
                std::filesystem::create_symlink(canary, directory / "settings.json.bak");
            StateStore store(directory);
            store.savePreferences(Preferences{});
            if (readDocument(canary) != content)
                throw std::runtime_error("unrelated canary was overwritten");
            if (!store.loadPreferences().warning.empty())
                throw std::runtime_error("saved preferences did not round-trip");
            struct stat information{};
            if (stat((directory / "settings.json").c_str(), &information) ||
                (information.st_mode & 0777) != 0600)
                throw std::runtime_error("saved preferences permissions were not 0600");
        }
        std::cout << "3 isolated symlink/hardlink overwrite attempts preserved the canary; "
                     "saved files mode0600\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
