#include "pomodoro/notifications.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace pomodoro {
namespace {
std::wstring duration(Millis ms) {
    const auto seconds = (std::max<Millis>)(0, ms) / 1000 + (ms > 0 && ms % 1000 != 0 ? 1 : 0);
    const auto two = [](Millis n) { return (n < 10 ? L"0" : L"") + std::to_wstring(n); };
    if (seconds >= 3600) return two(seconds / 3600) + L":" + two(seconds / 60 % 60) + L":" + two(seconds % 60);
    return two(seconds / 60) + L":" + two(seconds % 60);
}
bool safeSession(const std::wstring& session) {
    return !session.empty() && session.size() <= 96 &&
        std::all_of(session.begin(), session.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                   (c >= L'A' && c <= L'Z') || c == L'-';
        });
}
std::wstring escape(const std::wstring& text) {
    std::wstring result;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'\"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: if (c >= L' ' || c == L'\n' || c == L'\r' || c == L'\t') result += c;
        }
    }
    return result;
}
void setXml(Notification& notice, bool sound) {
    const std::wstring timer = notice.timer == TimerKind::Pomodoro ? L"pomodoro/" : L"countdown/";
    const std::wstring session(notice.sessionId.begin(), notice.sessionId.end());
    // Persisted state may contain a legacy, non-URI session ID. Never interpolate
    // it into an executable command or allow its notification to stop a session.
    const std::wstring token = safeSession(session) ? session : L"invalid";
    const auto target = timer + token;
    notice.xml = L"<toast activationType=\"protocol\" launch=\"pomodoro-native://open/" + target +
        L"\"><visual><binding template=\"ToastGeneric\"><text>" + escape(notice.title) +
        L"</text><text>" + escape(notice.body) + L"</text></binding></visual><actions>"
        L"<action content=\"Dismiss\" activationType=\"system\" arguments=\"dismiss\"/>"
        L"<action content=\"Stop\" activationType=\"protocol\" arguments=\"pomodoro-native://stop/" + target +
        L"\"/></actions>" + (sound ? std::wstring{} : L"<audio silent=\"true\"/>") + L"</toast>";
}
} // namespace

std::vector<Notification> completionNotifications(const TimerEngine& engine,
    const std::vector<CompletionEvent>& events, Millis now, bool recovery, bool sound) {
    std::vector<Notification> result;
    for (const auto timer : {TimerKind::Pomodoro, TimerKind::Countdown}) {
        const auto count = std::count_if(events.begin(), events.end(),
            [timer](const auto& e) { return e.timer == timer; });
        if (count == 0) continue;
        Notification n{};
        n.timer = timer;
        if (timer == TimerKind::Countdown) {
            n.sessionId = engine.countdown.sessionId;
            n.tag = L"countdown";
            n.title = L"Timer finished";
            n.body = L"Your " + duration(engine.countdown.durationMs) + L" timer has ended.";
        } else {
            const auto& p = engine.pomodoro;
            n.sessionId = p.sessionId;
            n.tag = L"pomodoro";
            const auto last = std::find_if(events.rbegin(), events.rend(),
                [](const auto& e) { return e.timer == TimerKind::Pomodoro; });
            if (p.status == Status::Completed) {
                n.title = L"Pomodoro complete";
                n.body = L"All " + std::to_wstring(p.config.cycles) + L" cycles and the final break are finished.";
            } else {
                n.title = last->phase == Phase::Focus ? L"Pomodoro: focus finished" : L"Pomodoro: break finished";
                if (recovery && count > 1) n.title = L"Pomodoro updated";
                const auto phase = p.phase == Phase::Focus ? L"Focus" : L"Break";
                n.body = std::wstring(phase) + (p.status == Status::Paused ? L" is paused with " : L" has ") +
                    duration(engine.pomodoroRemaining(now)) + L" remaining. Cycle " +
                    std::to_wstring(p.cycle) + L" of " + std::to_wstring(p.config.cycles) + L".";
            }
            if (recovery && count > 1) n.body += L" " + std::to_wstring(count) + L" phases ended while you were away.";
        }
        setXml(n, sound);
        result.push_back(std::move(n));
    }
    return result;
}

Notification testNotification(bool sound) {
    Notification n{TimerKind::Pomodoro, "notification-test", L"Pomodoro notification test",
        L"Notifications are working. Dismiss removes this message. Stop on this test does not affect your timers.", L"test", {}};
    setXml(n, sound);
    return n;
}

std::optional<NotificationAction> parseNotificationAction(const std::wstring& uri) {
    constexpr wchar_t prefix[] = L"pomodoro-native://";
    if (uri.size() > 180 || uri.compare(0, std::size(prefix) - 1, prefix) != 0) return std::nullopt;
    auto tail = uri.substr(std::size(prefix) - 1);
    NotificationCommand command;
    if (tail.compare(0, 5, L"open/") == 0) { command = NotificationCommand::Open; tail.erase(0, 5); }
    else if (tail.compare(0, 5, L"stop/") == 0) { command = NotificationCommand::Stop; tail.erase(0, 5); }
    else return std::nullopt;
    TimerKind timer;
    if (tail.compare(0, 9, L"pomodoro/") == 0) { timer = TimerKind::Pomodoro; tail.erase(0, 9); }
    else if (tail.compare(0, 10, L"countdown/") == 0) { timer = TimerKind::Countdown; tail.erase(0, 10); }
    else return std::nullopt;
    if (!safeSession(tail)) return std::nullopt;
    return NotificationAction{command, timer, std::string(tail.begin(), tail.end())};
}

bool matchesNotificationSession(const TimerEngine& engine, const NotificationAction& action) {
    return !action.sessionId.empty() && action.sessionId != "notification-test" && action.sessionId != "invalid" &&
        action.sessionId == (action.timer == TimerKind::Pomodoro ? engine.pomodoro.sessionId : engine.countdown.sessionId);
}

void NotificationOutbox::post(Notification notice, Millis now) {
    cancel(notice.timer);
    items_.push_back({std::move(notice), now, 0});
}
void NotificationOutbox::cancel(TimerKind timer) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [timer](const Pending& p) {
        return p.notification.timer == timer;
    }), items_.end());
}
void NotificationOutbox::discardStale(const TimerEngine& engine) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&engine](const Pending& p) {
        return !matchesNotificationSession(engine, {NotificationCommand::Stop, p.notification.timer, p.notification.sessionId});
    }), items_.end());
}
void NotificationOutbox::dispatch(Millis now, const std::function<bool(const Notification&)>& deliver) {
    for (auto entry = items_.begin(); entry != items_.end();) {
        if (now < entry->nextAttempt) { ++entry; continue; }
        ++entry->attempts;
        if (deliver(entry->notification) || entry->attempts >= 3) entry = items_.erase(entry);
        else {
            entry->nextAttempt = now > std::numeric_limits<Millis>::max() - 5000
                ? std::numeric_limits<Millis>::max() : now + 5000;
            ++entry;
        }
    }
}

} // namespace pomodoro
