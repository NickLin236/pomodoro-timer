#include "pomodoro/state_store.h"

#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pomodoro {
namespace {

constexpr std::size_t maxDocumentBytes = 64 * 1024;
constexpr int schemaVersion = 1;

[[noreturn]] void invalid(const std::string& detail) {
    throw std::runtime_error("Invalid JSON document: " + detail);
}

bool validUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 0x07; minimum = 0x10000; }
        else return false;
        if (i + count > text.size()) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

struct Json {
    enum class Type { Object, Array, String, Number, Boolean, Null } type = Type::Null;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    std::string scalar;
};

// Deliberately small parser: no dependency or network access is required. It
// accepts standard JSON syntax, rejects duplicate keys, and bounds input/depth.
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    Json parse() {
        if (text_.size() > maxDocumentBytes || !validUtf8(text_)) invalid("size or UTF-8 encoding");
        auto result = value(0);
        whitespace();
        if (position_ != text_.size()) invalid("trailing data");
        return result;
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
               text_[position_] == '\n' || text_[position_] == '\r')) ++position_;
    }
    bool take(char character) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == character) { ++position_; return true; }
        return false;
    }
    void require(char character) { if (!take(character)) invalid("unexpected token"); }
    unsigned hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            if (position_ == text_.size()) invalid("incomplete Unicode escape");
            const char c = text_[position_++];
            result <<= 4;
            if (c >= '0' && c <= '9') result += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') result += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') result += static_cast<unsigned>(c - 'A' + 10);
            else invalid("invalid Unicode escape");
        }
        return result;
    }
    static void appendUtf8(std::string& output, unsigned point) {
        if (point <= 0x7f) output += static_cast<char>(point);
        else if (point <= 0x7ff) {
            output += static_cast<char>(0xc0 | (point >> 6));
            output += static_cast<char>(0x80 | (point & 0x3f));
        } else if (point <= 0xffff) {
            output += static_cast<char>(0xe0 | (point >> 12));
            output += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (point & 0x3f));
        } else {
            output += static_cast<char>(0xf0 | (point >> 18));
            output += static_cast<char>(0x80 | ((point >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (point & 0x3f));
        }
    }
    std::string string() {
        require('"');
        std::string result;
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return result;
            if (c < 0x20) invalid("unescaped control character");
            if (c != '\\') { result += static_cast<char>(c); continue; }
            if (position_ == text_.size()) invalid("incomplete string escape");
            switch (text_[position_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                auto point = hex4();
                if (point >= 0xd800 && point <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u')
                        invalid("missing Unicode low surrogate");
                    position_ += 2;
                    const auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) invalid("invalid Unicode low surrogate");
                    point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                } else if (point >= 0xdc00 && point <= 0xdfff) invalid("unpaired Unicode surrogate");
                appendUtf8(result, point);
                break;
            }
            default: invalid("invalid string escape");
            }
        }
        invalid("unterminated string");
    }
    bool digit() const { return position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9'; }
    Json value(unsigned depth) {
        if (depth > 24) invalid("nesting too deep");
        whitespace();
        if (position_ == text_.size()) invalid("missing value");
        Json result;
        if (take('{')) {
            result.type = Json::Type::Object;
            if (take('}')) return result;
            do {
                auto key = string();
                require(':');
                auto member = value(depth + 1);
                if (!result.object.emplace(std::move(key), std::move(member)).second) invalid("duplicate member");
            } while (take(','));
            require('}');
            return result;
        }
        if (take('[')) {
            result.type = Json::Type::Array;
            if (take(']')) return result;
            do { result.array.push_back(value(depth + 1)); } while (take(','));
            require(']');
            return result;
        }
        if (text_[position_] == '"') { result.type = Json::Type::String; result.scalar = string(); return result; }
        for (const auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
            if (text_.substr(position_, literal.size()) == literal) {
                position_ += literal.size();
                result.type = literal == "null" ? Json::Type::Null : Json::Type::Boolean;
                result.scalar = literal;
                return result;
            }
        }
        const auto start = position_;
        if (text_[position_] == '-') ++position_;
        if (!digit()) invalid("invalid number");
        if (text_[position_] == '0') ++position_;
        else while (digit()) ++position_;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (!digit()) invalid("invalid fraction");
            while (digit()) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            if (!digit()) invalid("invalid exponent");
            while (digit()) ++position_;
        }
        result.type = Json::Type::Number;
        result.scalar = text_.substr(start, position_ - start);
        return result;
    }
};

const Json& field(const Json& object, const char* name) {
    if (object.type != Json::Type::Object) invalid("expected object");
    const auto found = object.object.find(name);
    if (found == object.object.end()) invalid(std::string("missing ") + name);
    return found->second;
}

template <typename Integer> Integer integer(const Json& value) {
    if (value.type != Json::Type::Number) invalid("expected integer");
    Integer result{};
    const auto conversion = std::from_chars(value.scalar.data(), value.scalar.data() + value.scalar.size(), result);
    if (conversion.ec != std::errc{} || conversion.ptr != value.scalar.data() + value.scalar.size()) invalid("integer range or syntax");
    return result;
}

std::string string(const Json& value) {
    if (value.type != Json::Type::String) invalid("expected string");
    return value.scalar;
}
bool boolean(const Json& value) {
    if (value.type != Json::Type::Boolean) invalid("expected boolean");
    return value.scalar == "true";
}
void checkVersion(const Json& document) {
    if (integer<int>(field(document, "version")) != schemaVersion) invalid("unsupported schema version");
}
Status readStatus(const Json& value) {
    const auto name = string(value);
    if (name == "idle") return Status::Idle;
    if (name == "running") return Status::Running;
    if (name == "paused") return Status::Paused;
    if (name == "completed") return Status::Completed;
    invalid("unknown timer status");
}
const char* statusName(Status status) {
    switch (status) {
    case Status::Idle: return "idle";
    case Status::Running: return "running";
    case Status::Paused: return "paused";
    case Status::Completed: return "completed";
    }
    invalid("unknown timer status");
}
PomodoroConfig readConfig(const Json& value) {
    PomodoroConfig result;
    result.focusMs = integer<Millis>(field(value, "focusMs"));
    result.breakMs = integer<Millis>(field(value, "breakMs"));
    result.cycles = integer<int>(field(value, "cycles"));
    if (!validateConfig(result)) invalid("Pomodoro settings out of range");
    return result;
}
void checkSessionId(const std::string& id, Status status) {
    if (id.size() > 128 || (status != Status::Idle && id.empty()) || !validUtf8(id)) invalid("session identifier");
    for (const auto c : id) if (static_cast<unsigned char>(c) < 0x20) invalid("session identifier control character");
}

AppState decodeState(const std::string& text) {
    const auto document = JsonParser(text).parse();
    checkVersion(document);
    AppState state;
    state.savedAtUtcMs = integer<Millis>(field(document, "savedAtUtcMs"));
    state.pomodoroNotificationSequence = integer<std::uint64_t>(field(document, "pomodoroNotificationSequence"));
    state.countdownNotificationSequence = integer<std::uint64_t>(field(document, "countdownNotificationSequence"));
    const auto& p = field(document, "pomodoro");
    state.pomodoro.sessionId = string(field(p, "sessionId"));
    state.pomodoro.status = readStatus(field(p, "status"));
    state.pomodoro.config = readConfig(field(p, "config"));
    const auto phase = string(field(p, "phase"));
    if (phase != "focus" && phase != "break") invalid("unknown phase");
    state.pomodoro.phase = phase == "focus" ? Phase::Focus : Phase::Break;
    state.pomodoro.cycle = integer<int>(field(p, "cycle"));
    state.pomodoro.phaseDurationMs = integer<Millis>(field(p, "phaseDurationMs"));
    state.pomodoro.deadlineUtcMs = integer<Millis>(field(p, "deadlineUtcMs"));
    state.pomodoro.remainingMs = integer<Millis>(field(p, "remainingMs"));
    state.pomodoro.transitionSequence = integer<std::uint64_t>(field(p, "transitionSequence"));
    const auto& c = field(document, "countdown");
    state.countdown.sessionId = string(field(c, "sessionId"));
    state.countdown.status = readStatus(field(c, "status"));
    state.countdown.durationMs = integer<Millis>(field(c, "durationMs"));
    state.countdown.deadlineUtcMs = integer<Millis>(field(c, "deadlineUtcMs"));
    state.countdown.remainingMs = integer<Millis>(field(c, "remainingMs"));
    state.countdown.transitionSequence = integer<std::uint64_t>(field(c, "transitionSequence"));

    const auto& ps = state.pomodoro;
    const auto& cs = state.countdown;
    checkSessionId(ps.sessionId, ps.status);
    checkSessionId(cs.sessionId, cs.status);
    if (state.savedAtUtcMs < 0 || ps.deadlineUtcMs < 0 || cs.deadlineUtcMs < 0) invalid("negative persisted UTC time");
    if (ps.cycle < 1 || ps.cycle > ps.config.cycles ||
        ps.phaseDurationMs != (ps.phase == Phase::Focus ? ps.config.focusMs : ps.config.breakMs) ||
        ps.remainingMs < 0 ||
        ps.transitionSequence > static_cast<std::uint64_t>(ps.config.cycles * 2)) invalid("inconsistent Pomodoro state");
    if (!validateDuration(cs.durationMs) || cs.remainingMs < 0 ||
        cs.transitionSequence > 1) invalid("inconsistent countdown state");
    if ((ps.status == Status::Paused && ps.remainingMs == 0) || (cs.status == Status::Paused && cs.remainingMs == 0))
        invalid("paused timer without remaining time");
    if ((ps.status == Status::Running && ps.remainingMs == 0) || (cs.status == Status::Running && cs.remainingMs == 0))
        invalid("running timer without remaining time");
    if ((ps.status == Status::Running && ps.deadlineUtcMs == 0) || (cs.status == Status::Running && cs.deadlineUtcMs == 0))
        invalid("running timer without deadline");
    if ((ps.status != Status::Running && ps.deadlineUtcMs != 0) || (cs.status != Status::Running && cs.deadlineUtcMs != 0))
        invalid("inactive timer with deadline");
    if (ps.status == Status::Completed && (ps.remainingMs != 0 || ps.phase != Phase::Break || ps.cycle != ps.config.cycles))
        invalid("inconsistent completed Pomodoro");
    if (cs.status == Status::Completed && cs.remainingMs != 0) invalid("inconsistent completed countdown");
    if (ps.status == Status::Idle && (ps.phase != Phase::Focus || ps.cycle != 1 || ps.remainingMs != ps.phaseDurationMs || ps.transitionSequence != 0))
        invalid("inconsistent idle Pomodoro");
    if (cs.status == Status::Idle && (cs.remainingMs != cs.durationMs || cs.transitionSequence != 0)) invalid("inconsistent idle countdown");
    const auto expectedPomodoroSequence = ps.status == Status::Completed ? static_cast<std::uint64_t>(ps.config.cycles * 2) :
        static_cast<std::uint64_t>(2 * (ps.cycle - 1) + (ps.phase == Phase::Break ? 1 : 0));
    if (ps.transitionSequence != expectedPomodoroSequence ||
        cs.transitionSequence != (cs.status == Status::Completed ? 1u : 0u)) invalid("inconsistent transition sequence");
    if (state.pomodoroNotificationSequence > ps.transitionSequence || state.countdownNotificationSequence > cs.transitionSequence)
        invalid("notification sequence exceeds transition sequence");
    if (ps.status == Status::Running) {
        // The remaining schedule must be representable before advance() can
        // safely walk all missed boundaries after a long absence.
        const auto laterCycles = static_cast<Millis>(ps.config.cycles - ps.cycle);
        const auto future = laterCycles * (ps.config.focusMs + ps.config.breakMs) +
            (ps.phase == Phase::Focus ? ps.config.breakMs : 0);
        if (ps.deadlineUtcMs > std::numeric_limits<Millis>::max() - future) invalid("schedule overflows UTC range");
    } else if (ps.status == Status::Paused) {
        // A backward clock change while exited can legitimately make frozen
        // remaining time longer than the captured nominal phase duration.
        const auto laterCycles = static_cast<Millis>(ps.config.cycles - ps.cycle);
        const auto future = laterCycles * (ps.config.focusMs + ps.config.breakMs) +
            (ps.phase == Phase::Focus ? ps.config.breakMs : 0);
        if (ps.remainingMs > std::numeric_limits<Millis>::max() - future) invalid("paused schedule overflows duration range");
    }
    return state;
}

Preferences decodePreferences(const std::string& text) {
    const auto document = JsonParser(text).parse();
    checkVersion(document);
    Preferences result;
    result.pomodoro = readConfig(field(document, "pomodoro"));
    result.countdownDurationMs = integer<Millis>(field(document, "countdownDurationMs"));
    if (!validateDuration(result.countdownDurationMs)) invalid("countdown duration out of range");
    result.soundEnabled = boolean(field(document, "soundEnabled"));
    result.startAtSignIn = boolean(field(document, "startAtSignIn"));
    // Schema 1 files written before taskbar status support have no such field.
    // Preserve their valid timer settings and opt into the new display default.
    const auto showStatus = document.object.find("showTaskbarStatus");
    if (showStatus != document.object.end()) result.showTaskbarStatus = boolean(showStatus->second);
    const auto mode = string(field(document, "backgroundMode"));
    if (mode != "trayOnly" && mode != "keepTaskbar") invalid("background mode");
    result.backgroundMode = mode == "trayOnly" ? BackgroundMode::TrayOnly : BackgroundMode::KeepTaskbar;
    const auto source = string(field(document, "taskbarSource"));
    if (source != "pomodoro" && source != "countdown") invalid("taskbar source");
    result.taskbarSource = source == "pomodoro" ? TaskbarSource::Pomodoro : TaskbarSource::Countdown;
    return result;
}

std::string quote(std::string_view value) {
    if (!validUtf8(value)) invalid("invalid UTF-8 string");
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::string encodeConfig(const PomodoroConfig& config) {
    return "{\"focusMs\":" + std::to_string(config.focusMs) + ",\"breakMs\":" + std::to_string(config.breakMs) +
        ",\"cycles\":" + std::to_string(config.cycles) + "}";
}
std::string encodeState(const AppState& state) {
    const auto& p = state.pomodoro;
    const auto& c = state.countdown;
    if (p.phase != Phase::Focus && p.phase != Phase::Break) invalid("unknown phase");
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "{\n  \"version\":1,\n  \"savedAtUtcMs\":" << state.savedAtUtcMs
         << ",\n  \"pomodoroNotificationSequence\":" << state.pomodoroNotificationSequence
         << ",\n  \"countdownNotificationSequence\":" << state.countdownNotificationSequence
         << ",\n  \"pomodoro\":{\"sessionId\":" << quote(p.sessionId)
         << ",\"status\":" << quote(statusName(p.status)) << ",\"config\":" << encodeConfig(p.config)
         << ",\"phase\":" << quote(p.phase == Phase::Focus ? "focus" : "break") << ",\"cycle\":" << p.cycle
         << ",\"phaseDurationMs\":" << p.phaseDurationMs << ",\"deadlineUtcMs\":" << p.deadlineUtcMs
         << ",\"remainingMs\":" << p.remainingMs << ",\"transitionSequence\":" << p.transitionSequence
         << "},\n  \"countdown\":{\"sessionId\":" << quote(c.sessionId) << ",\"status\":" << quote(statusName(c.status))
         << ",\"durationMs\":" << c.durationMs << ",\"deadlineUtcMs\":" << c.deadlineUtcMs
         << ",\"remainingMs\":" << c.remainingMs << ",\"transitionSequence\":" << c.transitionSequence << "}\n}\n";
    auto result = text.str();
    (void)decodeState(result); // Validate caller-owned state before touching disk.
    return result;
}
std::string encodePreferences(const Preferences& value) {
    if (value.backgroundMode != BackgroundMode::TrayOnly && value.backgroundMode != BackgroundMode::KeepTaskbar)
        invalid("background mode");
    if (value.taskbarSource != TaskbarSource::Pomodoro && value.taskbarSource != TaskbarSource::Countdown)
        invalid("taskbar source");
    const auto result = "{\n  \"version\":1,\n  \"pomodoro\":" + encodeConfig(value.pomodoro) +
        ",\n  \"countdownDurationMs\":" + std::to_string(value.countdownDurationMs) +
        ",\n  \"soundEnabled\":" + (value.soundEnabled ? "true" : "false") +
        ",\n  \"startAtSignIn\":" + (value.startAtSignIn ? "true" : "false") +
        ",\n  \"showTaskbarStatus\":" + (value.showTaskbarStatus ? "true" : "false") +
        ",\n  \"backgroundMode\":" + quote(value.backgroundMode == BackgroundMode::TrayOnly ? "trayOnly" : "keepTaskbar") +
        ",\n  \"taskbarSource\":" + quote(value.taskbarSource == TaskbarSource::Pomodoro ? "pomodoro" : "countdown") + "\n}\n";
    (void)decodePreferences(result);
    return result;
}

std::string readDocument(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file");
    std::string text;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        text.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (text.size() > maxDocumentBytes) throw std::runtime_error("File exceeds 64 KiB");
    }
    if (input.bad()) throw std::runtime_error("File read failed");
    return text;
}

template <typename T, typename Decoder>
LoadResult<T> load(const std::filesystem::path& path, Decoder decode) {
    LoadResult<T> result;
    const auto backup = std::filesystem::path(path).concat(".bak");
    std::string primaryError;
    bool primaryPresent = false;
    try {
        const auto directory = std::filesystem::status(path.parent_path());
        if (std::filesystem::exists(directory) && !std::filesystem::is_directory(directory))
            throw std::runtime_error("Storage path is not a directory");
        primaryPresent = std::filesystem::exists(path);
        if (primaryPresent) { result.value = decode(readDocument(path)); return result; }
    } catch (const std::exception& error) { primaryError = error.what(); }
    try {
        if (std::filesystem::exists(backup)) {
            result.value = decode(readDocument(backup));
            result.recoveredFromBackup = true;
            result.warning = path.filename().string() + " was unavailable or invalid; recovered the last known good backup.";
            return result;
        }
    } catch (const std::exception& error) {
        if (!primaryError.empty()) primaryError += "; ";
        primaryError += std::string("backup: ") + error.what();
    }
    if (primaryPresent || !primaryError.empty())
        result.warning = "Could not recover " + path.filename().string() + "; using safe defaults. " + primaryError;
    return result;
}

[[noreturn]] void ioFailure(const char* operation, unsigned long code) {
    throw std::runtime_error(std::string(operation) + " failed (system error " + std::to_string(code) + ")");
}

void atomicWrite(const std::filesystem::path& destination, const std::string& content) {
    static std::atomic<unsigned long> serial{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    std::filesystem::path temporary;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int file = -1;
#endif
    // A previous process can leave temporary files after a crash, and process
    // IDs are eventually reused. Retry exclusive-create collisions; cleanup
    // must own only a file that this invocation actually opened.
    for (int attempt = 0; attempt < 128; ++attempt) {
        temporary = destination;
        temporary += ".tmp-" + std::to_string(process) + "-" + std::to_string(++serial);
#ifdef _WIN32
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            ioFailure("Create temporary file", error);
#else
        file = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (file >= 0) break;
        if (errno != EEXIST) ioFailure("Create temporary file", static_cast<unsigned long>(errno));
#endif
    }
#ifdef _WIN32
    if (file == INVALID_HANDLE_VALUE) ioFailure("Create unique temporary file", ERROR_FILE_EXISTS);
#else
    if (file < 0) ioFailure("Create unique temporary file", static_cast<unsigned long>(EEXIST));
#endif
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temporary};
#ifdef _WIN32
    struct FileCloser { HANDLE handle; ~FileCloser() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } closer{file};
    DWORD written = 0;
    if (!WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size())
        ioFailure("Write temporary file", GetLastError());
    if (!FlushFileBuffers(file)) ioFailure("Flush temporary file", GetLastError());
    CloseHandle(file);
    closer.handle = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ioFailure("Replace document", GetLastError());
#else
    struct FileCloser { int fd; ~FileCloser() { if (fd >= 0) ::close(fd); } } closer{file};
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(file, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) ioFailure("Write temporary file", static_cast<unsigned long>(errno));
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(file) != 0) ioFailure("Flush temporary file", static_cast<unsigned long>(errno));
    if (::close(file) != 0) { closer.fd = -1; ioFailure("Close temporary file", static_cast<unsigned long>(errno)); }
    closer.fd = -1;
    if (::rename(temporary.c_str(), destination.c_str()) != 0) ioFailure("Replace document", static_cast<unsigned long>(errno));
    const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) ioFailure("Open storage directory", static_cast<unsigned long>(errno));
    FileCloser directoryCloser{directory};
    if (::fsync(directory) != 0) ioFailure("Flush storage directory", static_cast<unsigned long>(errno));
#endif
}

template <typename Decoder>
void save(const std::filesystem::path& path, const std::string& text, Decoder decode) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const auto backup = std::filesystem::path(path).concat(".bak");
        std::string oldGood;
        try { oldGood = readDocument(path); (void)decode(oldGood); }
        catch (const std::exception&) { oldGood.clear(); }
        if (!oldGood.empty()) atomicWrite(backup, oldGood);
        else {
            bool goodBackup = false;
            try { (void)decode(readDocument(backup)); goodBackup = true; }
            catch (const std::exception&) {}
            // Establish a backup on the first save, but never replace a good
            // backup with a corrupt primary document.
            if (!goodBackup) atomicWrite(backup, text);
        }
        atomicWrite(path, text);
    } catch (const std::exception& error) {
        throw std::runtime_error("Could not save " + path.filename().string() + ": " + error.what());
    }
}

} // namespace

StateStore::StateStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
const std::filesystem::path& StateStore::directory() const noexcept { return directory_; }
LoadResult<AppState> StateStore::loadState() const { return load<AppState>(directory_ / "state.json", decodeState); }
LoadResult<Preferences> StateStore::loadPreferences() const { return load<Preferences>(directory_ / "settings.json", decodePreferences); }
void StateStore::saveState(const AppState& state) const { save(directory_ / "state.json", encodeState(state), decodeState); }
void StateStore::savePreferences(const Preferences& preferences) const { save(directory_ / "settings.json", encodePreferences(preferences), decodePreferences); }

} // namespace pomodoro
