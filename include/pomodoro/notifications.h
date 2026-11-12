#pragma once

#include "pomodoro/timer_engine.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pomodoro {

enum class NotificationCommand { Open, Stop };
struct NotificationAction {
    NotificationCommand command;
    TimerKind timer;
    std::string sessionId;
};

struct Notification {
    TimerKind timer;
    std::string sessionId;
    std::wstring title;
    std::wstring body;
    std::wstring tag;
    std::wstring xml;
};

// Group overdue completions per timer, retaining only that timer's current state.
std::vector<Notification> completionNotifications(const TimerEngine& engine,
    const std::vector<CompletionEvent>& events, Millis now, bool recovery, bool sound = true);
Notification testNotification(bool sound = true);
std::optional<NotificationAction> parseNotificationAction(const std::wstring& uri);
bool matchesNotificationSession(const TimerEngine& engine, const NotificationAction& action);

// Failed requests retain the latest notice for each timer, with bounded retries.
// The caller supplies time and transport; this type has no clock, storage or UI.
class NotificationOutbox {
public:
    void post(Notification notification, Millis now);
    void cancel(TimerKind timer);
    void discardStale(const TimerEngine& engine);
    void dispatch(Millis now, const std::function<bool(const Notification&)>& deliver);
    bool pending() const noexcept { return !items_.empty(); }
private:
    struct Pending { Notification notification; Millis nextAttempt; unsigned attempts = 0; };
    std::vector<Pending> items_;
};

} // namespace pomodoro
