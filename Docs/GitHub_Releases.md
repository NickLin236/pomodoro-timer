# GitHub upload and executable releases

Repository: [NickLin236/pomodoro-timer](https://github.com/NickLin236/pomodoro-timer).

The consumer download is a standalone Windows 10 x64 `Pomodoro.exe`. It embeds icons, version information, and its manifest; state/settings are created in LocalAppData. Users download this one executable and run it directly. The optional ZIP contains installation/uninstallation scripts and documentation.

The permanent latest-release download link will be:

```text
https://github.com/NickLin236/pomodoro-timer/releases/latest/download/Pomodoro.exe
```

That URL works once a successful published release contains `Pomodoro.exe`. It follows GitHub's [latest-release asset URL format](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases).

## Upload the source

Upload only the project source, resources, tests, docs, CMake files, and `.github` workflows. Generated binaries are release attachments. `.gitignore` excludes build output and workspace configuration. A source-only ZIP prepared with this project includes the hidden `.github`, `.gitignore`, and `.gitattributes` entries.

If the upload has not already been completed by the assistant, extract the prepared source ZIP into a normal writable folder. Install Git and [GitHub CLI](https://cli.github.com/) on your development machine, then run these commands from the extracted folder. They target the existing empty repository supplied above:

```powershell
gh auth login --hostname github.com --git-protocol https
gh auth setup-git
git init
git add .
git commit -m "Add native Pomodoro application and release automation"
git branch -M main
git remote add origin https://github.com/NickLin236/pomodoro-timer.git
git push -u origin main
```

If the project is already a Git checkout, retain its history and existing remote instead of running the initialization commands. These commands do not overwrite existing remote history; an existing nonempty repository must be integrated first.

GitHub runs the CI workflow for the initial push. It builds with MSVC on Windows x64, runs the automated tests, and keeps build artifacts on the workflow run.

## Publish version 1.0.0

After uploading the source:

```powershell
git tag v1.0.0
git push origin v1.0.0
```

The release workflow checks that the tag matches the CMake and embedded resource versions, builds the Windows Release executable with the static CRT, and runs the tests. It publishes a GitHub release only after the checks succeed. The release includes a direct `Pomodoro.exe` asset, the offline ZIP, and `SHA256SUMS.txt`.

Watch **Actions** in the repository for build/test results. After the Release workflow succeeds, open **Releases** and download `Pomodoro.exe`. The top-level README's download link then works. A GitHub-hosted Windows build does not replace the real Windows 10 shell/performance checklist in this repository.

For later versions, update `project(Pomodoro VERSION ...)` in `CMakeLists.txt`, the file/product versions in `resources/pomodoro.rc`, and the assembly version in `resources/pomodoro.manifest`. Commit and push the changes, then push the matching new `vX.Y.Z` tag. Keep published release tags fixed.

## Upload an already-built executable manually

If Actions is unavailable, you can attach the prepared `Pomodoro.exe` to a release using the repository's **Releases → Draft a new release** page. Choose a tag identifying the matching source, attach `Pomodoro.exe` and `SHA256SUMS.txt`, then publish. Do not attach the source ZIP as a substitute for the executable.

The local binary produced in this Linux environment is a MinGW Windows x64 cross-build. GitHub Actions produces the intended MSVC Release build. Local structural checks confirm that the cross-build imports only Windows system DLLs, but it has not been executed on Windows here. See [validation](Validation.md).
