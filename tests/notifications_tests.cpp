#include "pomodoro/notifications.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pomodoro;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

std::size_t occurrences(const std::wstring& text, const std::wstring& fragment) {
    std::size_t count = 0;
    for (auto position = text.find(fragment); position != std::wstring::npos;
         position = text.find(fragment, position + fragment.size())) ++count;
    return count;
}

Notification noticeFor(TimerKind timer, const std::string& session, const std::wstring& title) {
    Notification notice{};
    notice.timer = timer;
    notice.sessionId = session;
    notice.title = title;
    return notice;
}

void eachTimerDescribesOnlyItsOwnCompletion() {
    TimerEngine engine;
    CHECK(engine.startPomodoro({2'000, 1'000, 2}, 0, "pomo-1"));
    CHECK(engine.startCountdown(1'000, 0, "timer-1"));
    const auto timerEvents = engine.advance(1'000);
    CHECK(timerEvents.size() == 1);
    const auto timer = completionNotifications(engine, timerEvents, 1'000, false);
    CHECK(timer.size() == 1);
    CHECK(timer[0].timer == TimerKind::Countdown && timer[0].sessionId == "timer-1");
    CHECK(timer[0].title == L"Timer finished");
    CHECK(timer[0].body.find(L"00:01") != std::wstring::npos);
    CHECK(timer[0].body.find(L"Focus") == std::wstring::npos);
    CHECK(timer[0].body.find(L"Break") == std::wstring::npos);
    CHECK(timer[0].body.find(L"Cycle") == std::wstring::npos);

    const auto focusEvents = engine.advance(2'000);
    const auto focus = completionNotifications(engine, focusEvents, 2'000, false);
    CHECK(focus.size() == 1);
    CHECK(focus[0].timer == TimerKind::Pomodoro && focus[0].sessionId == "pomo-1");
    CHECK(focus[0].title.find(L"focus finished") != std::wstring::npos);
    CHECK(focus[0].body.find(L"Break has 00:01 remaining") != std::wstring::npos);
    CHECK(focus[0].body.find(L"Cycle 1 of 2") != std::wstring::npos);
    CHECK(focus[0].body.find(L"timer") == std::wstring::npos);

    const auto breakEvents = engine.advance(3'000);
    const auto nextFocus = completionNotifications(engine, breakEvents, 3'000, false);
    CHECK(nextFocus.size() == 1);
    CHECK(nextFocus[0].title.find(L"break finished") != std::wstring::npos);
    CHECK(nextFocus[0].body.find(L"Focus has 00:02 remaining") != std::wstring::npos);
    CHECK(nextFocus[0].body.find(L"Cycle 2 of 2") != std::wstring::npos);

    const auto remaining = engine.advance(6'000);
    const auto finished = completionNotifications(engine, remaining, 6'000, false);
    CHECK(finished.size() == 1);
    CHECK(finished[0].title == L"Pomodoro complete");
    CHECK(finished[0].body.find(L"All 2 cycles") != std::wstring::npos);
    CHECK(finished[0].body.find(L"final break") != std::wstring::npos);
    CHECK(completionNotifications(engine, {}, 6'000, false).empty());
}

void overdueAndSimultaneousEventsStayGroupedByTimer() {
    TimerEngine engine;
    CHECK(engine.startPomodoro({2'000, 1'000, 3}, 0, "pomo-recovery"));
    CHECK(engine.startCountdown(1'000, 0, "timer-recovery"));
    const auto events = engine.advance(4'000);
    CHECK(events.size() == 3);
    const auto notices = completionNotifications(engine, events, 4'000, true);
    CHECK(notices.size() == 2);
    const auto pomo = std::find_if(notices.begin(), notices.end(), [](const Notification& n) {
        return n.timer == TimerKind::Pomodoro;
    });
    const auto timer = std::find_if(notices.begin(), notices.end(), [](const Notification& n) {
        return n.timer == TimerKind::Countdown;
    });
    CHECK(pomo != notices.end() && timer != notices.end());
    CHECK(pomo->title == L"Pomodoro updated");
    CHECK(pomo->body.find(L"2 phases ended while you were away") != std::wstring::npos);
    CHECK(pomo->body.find(L"Focus has 00:01 remaining") != std::wstring::npos);
    CHECK(pomo->body.find(L"Cycle 2 of 3") != std::wstring::npos);
    CHECK(pomo->body.find(L"timer") == std::wstring::npos);
    CHECK(timer->title == L"Timer finished");
    CHECK(timer->body.find(L"phases") == std::wstring::npos);
    CHECK(timer->body.find(L"Cycle") == std::wstring::npos);
    CHECK(timer->body.find(L"Focus") == std::wstring::npos);
    CHECK(timer->tag != pomo->tag);

    TimerEngine simultaneous;
    CHECK(simultaneous.startPomodoro({1'000, 1'000, 1}, 0, "pomo-same"));
    CHECK(simultaneous.startCountdown(1'000, 0, "timer-same"));
    const auto normal = completionNotifications(simultaneous, simultaneous.advance(1'000), 1'000, false);
    CHECK(normal.size() == 2);
    CHECK(normal[0].title.find(L"focus finished") != std::wstring::npos);
    CHECK(normal[1].title == L"Timer finished");
}

void actionButtonsAndSoundUseSafeXml() {
    TimerEngine engine;
    CHECK(engine.startPomodoro({1'000, 1'000, 1}, 0, "session-12"));
    const auto events = engine.advance(1'000);
    const auto normal = completionNotifications(engine, events, 1'000, false);
    const auto silent = completionNotifications(engine, events, 1'000, false, false);
    CHECK(normal.size() == 1 && silent.size() == 1);
    const auto& xml = normal[0].xml;
    CHECK(occurrences(xml, L"<action ") == 2);
    CHECK(xml.find(L"content=\"Dismiss\" activationType=\"system\" arguments=\"dismiss\"") != std::wstring::npos);
    CHECK(xml.find(L"content=\"Stop\" activationType=\"protocol\"") != std::wstring::npos);
    CHECK(xml.find(L"pomodoro-native://stop/pomodoro/session-12") != std::wstring::npos);
    CHECK(xml.find(L"pomodoro-native://open/pomodoro/session-12") != std::wstring::npos);
    CHECK(xml.find(L"<audio silent=\"true\"/>") == std::wstring::npos);
    CHECK(silent[0].xml.find(L"<audio silent=\"true\"/>") != std::wstring::npos);
    CHECK(testNotification(false).xml.find(L"<audio silent=\"true\"/>") != std::wstring::npos);

    // Older state files can legally contain non-URI session IDs. A quote or
    // markup in such an ID must never become a toast attribute or a command.
    const std::string hostile = "legacy\"><action arguments=\"evil&'";
    engine.pomodoro.sessionId = hostile;
    const auto legacy = completionNotifications(engine, events, 1'000, false);
    CHECK(legacy.size() == 1);
    CHECK(occurrences(legacy[0].xml, L"<action ") == 2);
    CHECK(legacy[0].xml.find(L"evil") == std::wstring::npos);
    CHECK(legacy[0].xml.find(L"pomodoro-native://stop/pomodoro/invalid") != std::wstring::npos);
    const auto invalid = parseNotificationAction(L"pomodoro-native://stop/pomodoro/invalid");
    CHECK(invalid && !matchesNotificationSession(engine, *invalid));
}

void onlyExactApplicationUrisAreAccepted() {
    const auto open = parseNotificationAction(L"pomodoro-native://open/pomodoro/Ab9-42");
    CHECK(open && open->command == NotificationCommand::Open);
    CHECK(open->timer == TimerKind::Pomodoro && open->sessionId == "Ab9-42");
    const auto stop = parseNotificationAction(L"pomodoro-native://stop/countdown/123-2");
    CHECK(stop && stop->command == NotificationCommand::Stop);
    CHECK(stop->timer == TimerKind::Countdown && stop->sessionId == "123-2");
    const std::vector<std::wstring> rejected{
        L"", L"https://stop/pomodoro/a", L"pomodoro-native:/stop/pomodoro/a",
        L"POMODORO-NATIVE://stop/pomodoro/a", L"pomodoro-native://exit/pomodoro/a",
        L"pomodoro-native://stop/timer/a", L"pomodoro-native://stop/pomodoro/",
        L"pomodoro-native://stop/pomodoro/a/b", L"pomodoro-native://stop/pomodoro/a/",
        L"pomodoro-native://stop/pomodoro/../a", L"pomodoro-native://stop/pomodoro/a_b",
        L"pomodoro-native://stop/pomodoro/a?x=1", L"pomodoro-native://stop/pomodoro/a#stop",
        L"pomodoro-native://stop/pomodoro/%22", L"pomodoro-native://stop/pomodoro/a\"",
        L"pomodoro-native://stop/pomodoro/a'", L"pomodoro-native://stop/pomodoro/a b",
        L" pomodoro-native://stop/pomodoro/a", L"pomodoro-native://stop/pomodoro/a\n",
        L"pomodoro-native://stop/pomodoro/a --background", L"pomodoro-native://stop/pomodoro/C:\\a",
        L"pomodoro-native://stop/pomodoro/a\\b", L"pomodoro-native://stop/pomodoro/\u4e2d",
        L"pomodoro-native://stop/pomodoro/" + std::wstring(97, L'a'),
        std::wstring(181, L'a')
    };
    for (const auto& uri : rejected) CHECK(!parseNotificationAction(uri));
    CHECK(parseNotificationAction(L"pomodoro-native://stop/countdown/" + std::wstring(96, L'a')));
}

void staleAndTestActionsCannotStopCurrentSessions() {
    TimerEngine engine;
    CHECK(engine.startPomodoro({1'000, 1'000, 1}, 0, "current-pomo"));
    CHECK(engine.startCountdown(1'000, 0, "current-timer"));
    CHECK(matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, "current-pomo"}));
    CHECK(matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Countdown, "current-timer"}));
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, "old-pomo"}));
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Countdown, "current-pomo"}));
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, "notification-test"}));
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, ""}));
    engine.resetPomodoro({1'000, 1'000, 1});
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, "current-pomo"}));
    CHECK(engine.startPomodoro({1'000, 1'000, 1}, 0, "new-pomo"));
    CHECK(!matchesNotificationSession(engine, {NotificationCommand::Stop, TimerKind::Pomodoro, "current-pomo"}));
}

void latestNoticeReplacesOnlyItsOwnTimer() {
    NotificationOutbox outbox;
    outbox.post(noticeFor(TimerKind::Pomodoro, "pomo", L"focus finished"), 0);
    outbox.post(noticeFor(TimerKind::Countdown, "timer", L"timer finished"), 0);
    outbox.post(noticeFor(TimerKind::Pomodoro, "pomo", L"break finished"), 0);
    std::vector<std::wstring> delivered;
    outbox.dispatch(0, [&](const Notification& n) { delivered.push_back(n.title); return true; });
    CHECK(delivered.size() == 2);
    CHECK(std::find(delivered.begin(), delivered.end(), L"focus finished") == delivered.end());
    CHECK(std::find(delivered.begin(), delivered.end(), L"break finished") != delivered.end());
    CHECK(std::find(delivered.begin(), delivered.end(), L"timer finished") != delivered.end());
    CHECK(!outbox.pending());
    outbox.dispatch(10'000, [&](const Notification&) { throw std::runtime_error("already delivered"); return false; });
}

void rejectedRequestsRetryAtFiveAndTenSecondsThenStop() {
    NotificationOutbox outbox;
    outbox.post(noticeFor(TimerKind::Pomodoro, "pomo", L"focus finished"), 1'000);
    outbox.post(noticeFor(TimerKind::Countdown, "timer", L"timer finished"), 1'000);
    int rejected = 0;
    int accepted = 0;
    const auto deliver = [&](const Notification& n) {
        if (n.timer == TimerKind::Countdown) { ++accepted; return true; }
        ++rejected;
        return false;
    };
    outbox.dispatch(999, deliver);
    CHECK(rejected == 0 && accepted == 0);
    outbox.dispatch(1'000, deliver);
    CHECK(rejected == 1 && accepted == 1 && outbox.pending());
    outbox.dispatch(5'999, deliver);
    CHECK(rejected == 1 && accepted == 1);
    outbox.dispatch(6'000, deliver);
    CHECK(rejected == 2 && accepted == 1 && outbox.pending());
    outbox.dispatch(10'999, deliver);
    CHECK(rejected == 2);
    outbox.dispatch(11'000, deliver);
    CHECK(rejected == 3 && accepted == 1 && !outbox.pending());
    outbox.dispatch(100'000, deliver);
    CHECK(rejected == 3 && accepted == 1);

    // A later phase gets a fresh immediate attempt even after an older notice
    // has exhausted its budget; successful retries are removed immediately.
    outbox.post(noticeFor(TimerKind::Pomodoro, "pomo", L"break finished"), 100'000);
    int attempts = 0;
    outbox.dispatch(100'000, [&](const Notification&) { return ++attempts == 2; });
    CHECK(outbox.pending());
    outbox.dispatch(105'000, [&](const Notification&) { return ++attempts == 2; });
    CHECK(attempts == 2 && !outbox.pending());
}

void resetAndNewSessionsDiscardOldQueuedActions() {
    TimerEngine engine;
    CHECK(engine.startPomodoro({1'000, 1'000, 1}, 0, "old-pomo"));
    CHECK(engine.startCountdown(1'000, 0, "timer"));
    NotificationOutbox outbox;
    outbox.post(noticeFor(TimerKind::Pomodoro, "old-pomo", L"old phase"), 0);
    outbox.post(noticeFor(TimerKind::Countdown, "timer", L"timer completion"), 0);
    engine.resetPomodoro({1'000, 1'000, 1});
    outbox.discardStale(engine);
    CHECK(outbox.pending());
    int count = 0;
    outbox.dispatch(0, [&](const Notification& n) {
        CHECK(n.timer == TimerKind::Countdown);
        ++count;
        return true;
    });
    CHECK(count == 1 && !outbox.pending());

    outbox.post(noticeFor(TimerKind::Pomodoro, "old-pomo", L"old phase"), 0);
    CHECK(engine.startPomodoro({1'000, 1'000, 1}, 0, "new-pomo"));
    outbox.discardStale(engine);
    CHECK(!outbox.pending());
    outbox.post(noticeFor(TimerKind::Pomodoro, "new-pomo", L"current phase"), 0);
    outbox.post(noticeFor(TimerKind::Countdown, "timer", L"timer completion"), 0);
    outbox.cancel(TimerKind::Countdown);
    outbox.dispatch(0, [&](const Notification& n) {
        CHECK(n.timer == TimerKind::Pomodoro && n.sessionId == "new-pomo");
        ++count;
        return true;
    });
    CHECK(count == 2 && !outbox.pending());
}

} // namespace

int main() {
    try {
        eachTimerDescribesOnlyItsOwnCompletion();
        overdueAndSimultaneousEventsStayGroupedByTimer();
        actionButtonsAndSoundUseSafeXml();
        onlyExactApplicationUrisAreAccepted();
        staleAndTestActionsCannotStopCurrentSessions();
        latestNoticeReplacesOnlyItsOwnTimer();
        rejectedRequestsRetryAtFiveAndTenSecondsThenStop();
        resetAndNewSessionsDiscardOldQueuedActions();
        std::cout << "8 notification scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
