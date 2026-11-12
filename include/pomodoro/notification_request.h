#pragma once

#include <cstdint>

namespace pomodoro {

enum class NotificationRequestState { Accepted, SettingsReadFailed, Blocked, SendFailed };

struct NotificationRequestResult {
    NotificationRequestState state;
    std::int32_t status;
    int setting;
};

// ERROR_NOT_FOUND provides no NotificationSetting. Windows can return this
// platform error around desktop app registration; it is not an explicit block.
// Attempt delivery in that case and use its actual result.
// Explicit disabled settings and all other lookup errors remain authoritative.
// Callers inject the Windows operations so this flow is testable without the
// notification service, registration, or changes to a user's preferences.
template<class QuerySettings, class Send>
NotificationRequestResult requestNotification(QuerySettings&& querySettings, Send&& send) {
    int setting = 0;
    const std::int32_t queryStatus = querySettings(setting);
    if (queryStatus < 0 && static_cast<std::uint32_t>(queryStatus) != 0x80070490u)
        return {NotificationRequestState::SettingsReadFailed, queryStatus, setting};
    if (queryStatus >= 0 && setting != 0)
        return {NotificationRequestState::Blocked, queryStatus, setting};
    const std::int32_t sendStatus = send();
    return {sendStatus < 0 ? NotificationRequestState::SendFailed : NotificationRequestState::Accepted,
        sendStatus, setting};
}

} // namespace pomodoro
