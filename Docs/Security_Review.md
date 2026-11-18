# Security review — 1.2.5

Reviewed November 17–18, 2026. Scope: the source at base commit `b42dfea` and the local 1.2.5 changes, local adversarial tests, installers, release workflows, and Windows executable protection metadata. This is a source-assisted local penetration assessment, not a completed Windows desktop penetration test or a security certification.

No code-execution, memory-corruption, shell-injection, or privilege-escalation exploit was demonstrated in the tested scope. One executable hardening gap was confirmed; two other attack surfaces received preventive changes.

## Offline threat model

The app has no application networking client, listener, telemetry, updater, or downloaded runtime dependency. Source review found no socket/HTTP API usage. Windows notifications, accessibility and shell integration call operating-system services. Their implementation and any operating-system traffic are outside this application's control.

Offline operation still exposes local saved files, command-line/protocol inputs, Windows messages, DLL loading, per-user registration, installation scripts and release artifacts. A website or another application can ask Windows to open a registered URI scheme; this does not require the timer to make a network request. Microsoft documents that [URI activation can contain arbitrary untrusted input](https://learn.microsoft.com/en-us/windows/apps/develop/launch/handle-uri-activation).

The app runs as the current user, requests no elevation, and stores data and registrations in that user's profile/HKCU. A process that already controls that account can modify its files or close its windows; those actions are not treated as privilege-escalation findings. No third-party service or remote host was attacked.

## Findings and changes

| Finding | Evidence and impact | Change |
| --- | --- | --- |
| Confirmed ASLR hardening gap in the local 1.2.4 MinGW executable | PE `DllCharacteristics=0`, stripped relocations, no base relocation directory. This weakens exploitation resistance; it does not establish a memory-corruption exploit. The hosted MSVC binary was not inspected. | Explicit Windows linker protection flags, retained relocations, and an actual-file PE check before Windows CI tests and packaging. |
| Predictable notification action IDs; low-impact integrity risk | IDs were UTC milliseconds plus a counter. A party able to trigger protocol activation and guess a current ID could request Stop. Browser/OS prompts and an exact live-session match still apply. No end-to-end website exploit was demonstrated. | New sessions use 128 bits from the OS CSPRNG, encoded as 32 hexadecimal characters. On entropy failure, the controller rejects the new session before changing timer state or saving that session. |
| Implicit DLL search hardening opportunity | Dynamic DLL loads already specify System32, but implicit imports rely on loader policy. A planted-DLL execution test on the target Windows machine was not performed. | MSVC builds use `/DEPENDENTLOADFLAG:0x800`; CI verifies its load-configuration field. The restriction is supported on Windows 10 RS1 and later. MinGW builds do not claim this additional loader restriction. |

Windows x64 already enforces DEP; a missing NX compatibility flag in the old executable does **not** mean DEP was disabled. See Microsoft's [exploit protection reference](https://learn.microsoft.com/defender-endpoint/exploit-protection-reference). Explicit linker options follow the [GNU linker documentation](https://sourceware.org/binutils/docs/ld/Options.html) and Microsoft's [dependent-load restriction documentation](https://learn.microsoft.com/en-us/cpp/build/reference/dependentloadflag?view=msvc-170).

Legacy saved sessions keep their existing identifiers for compatibility. Reset and start a new timer after upgrading to obtain a random ID. Tokens are stored in the user profile and notification links; they do not protect against malicious software already running as that user.

## Adversarial checks

- **100,950 persistence inputs:** malformed JSON, numeric extremes, invalid encodings, excess nesting and oversized documents. The parser accepted 5,559 documents; 224 decoded states and 101 decoded settings passed their schemas. Valid states were exercised through timer operations at nine clock values, including signed 64-bit limits. No AddressSanitizer or UndefinedBehaviorSanitizer finding.
- **1,000,000 notification URI mutations:** Unicode, embedded NULs, quotes, delimiters, insertions, deletions and length changes. Every accepted payload reconstructed exactly as a canonical allowlisted URI. No sanitizer finding. The regular notification suite adds 20,000 mutations and systematic dangerous-character insertions.
- **Three filesystem-link overwrite attempts:** primary symlink, primary hardlink and backup symlink. An unrelated canary survived; replacement files had POSIX mode `0600`. These tests establish Linux behavior, not Windows reparse-point behavior.
- **Five portable CTest suites:** pass in Release and with address/undefined-behavior sanitizers. Controller coverage includes random IDs across restart with a frozen clock, stale/cross-timer Stop isolation, legacy compatibility and injected entropy failure without controller state writes. The GUI can save edited preferences before requesting a start; that is separate from the rejected session.
- **Installers and workflows:** no verified command injection or privilege escalation found. Scripts operate on per-user paths and HKCU. Workflows pin action revisions, keep release-write permissions out of PR builds, avoid inserting untrusted expressions directly into shell code, and verify release checksums and source metadata.

The JSON parser already limits documents to 64 KiB and nesting to 24, rejects duplicate keys and invalid UTF-8/surrogates, checks integer ranges and validates restored schedules. Protocol parsing requires exact actions, timer names and an ASCII session allowlist; extra arguments, percent escapes, paths and query strings are rejected. Executable and protocol arguments are quoted without invoking a command shell.

In the source checkout or source ZIP, `tests/security/README.md` contains reproduction commands and the maintained harnesses. They are not required by the standalone executable. LeakSanitizer cannot run under this sandbox's ptrace environment and was disabled; address and undefined-behavior checks remained enabled. Observing distinct random samples is a regression check, not proof of cryptographic quality; security relies on the [Windows CSPRNG contract](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom).

## Executable verification

The local 1.2.5 app and all seven test executables cross-compile and pass the PE protection checker. The app has `DllCharacteristics=0x160`, retained `.reloc` data, and no section marked both writable and executable. Its import table contains only the ten existing Windows system DLLs; the entropy provider is loaded explicitly from System32. No networking import was added.

GNU ld 2.34 crashed while generating relocations for the complete app. The final cross-build uses locally built GNU ld 2.43.1; protection flags were retained. This is a build-tool compatibility issue, not an application crash. The local MinGW executable does not include MSVC's additional implicit-DLL search restriction.

## Remaining validation

Native Windows tests cannot execute in this Linux environment. The release workflow must build with MSVC, verify all eight executables, and run all seven Windows suites. Real Windows checks still include notification activation, malicious DLL placement with an inert test DLL in an isolated directory, reparse points and filesystem ACLs, and taskbar/accessibility behavior. No successful Windows attack simulation or MSVC protection check is claimed here.

The PE verifier checks protection metadata, bounds of raw section data, relocation presence, and writable/executable sections. It is not a general executable validator or a signature verification system. Release checksums identify bytes; they do not replace code signing or protect against compromise of the repository/release account. Releases remain unsigned.

A process already controlling the same user's data directory can disrupt blocking file I/O, and a broken accessibility provider can stall the single capped background reader. The timer UI does not wait for that reader; taskbar status hides without reliable measurements. No cross-account privilege boundary or remote exploit was demonstrated for these limitations.
