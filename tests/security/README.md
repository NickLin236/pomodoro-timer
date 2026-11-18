# Local adversarial harnesses

Run these from the repository root on Linux with GCC. They use fixed seeds and
require no network or production profile. The parser harness deliberately
includes the persistence implementation to reach its private JSON decoder.
Do not also link `state_store.cpp` or `pomodoro_core` into that executable.

```sh
mkdir -p /tmp/pomodoro-security-harnesses
g++ -std=c++2a -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer -fno-pie -no-pie -Iinclude \
  tests/security/persistence_fuzz.cpp src/core/timer_engine.cpp \
  -o /tmp/pomodoro-security-harnesses/persistence_fuzz
g++ -std=c++2a -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer -fno-pie -no-pie -Iinclude \
  tests/security/persistence_links.cpp src/core/timer_engine.cpp \
  -o /tmp/pomodoro-security-harnesses/persistence_links
g++ -std=c++2a -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer -fno-pie -no-pie -Iinclude \
  tests/security/uri_fuzz.cpp src/app/notifications.cpp src/core/timer_engine.cpp \
  -o /tmp/pomodoro-security-harnesses/uri_fuzz
ASAN_OPTIONS=detect_leaks=0 /tmp/pomodoro-security-harnesses/persistence_fuzz
ASAN_OPTIONS=detect_leaks=0 /tmp/pomodoro-security-harnesses/persistence_links
ASAN_OPTIONS=detect_leaks=0 /tmp/pomodoro-security-harnesses/uri_fuzz
```

LeakSanitizer is disabled because the audit sandbox uses ptrace, which it does
not support. AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.
On a host that supports leak detection, omit `ASAN_OPTIONS=detect_leaks=0`.

The persistence fuzzer tries 100,950 malformed, oversized, deeply nested and
numeric-boundary inputs, including 100,000 mutations. Decoded timer states are
then exercised at nine clock values, including `int64` limits. The URI fuzzer
tries 1,000,000 mutations and requires every accepted payload to reconstruct
exactly as a complete safe URI. Acceptance counts can vary with `wchar_t` width;
the canonical-input invariant must always hold.

The links harness creates a private temporary directory and tests primary
symlink, primary hardlink and backup symlink cases against an unrelated canary.
It verifies replacement files have mode `0600`, then removes its temporary
directory. This establishes POSIX behavior only; Windows reparse-point tests
remain separate native checks.
