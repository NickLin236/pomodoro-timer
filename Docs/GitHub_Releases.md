# GitHub upload and executable releases

Repository: [NickLin236/pomodoro-timer](https://github.com/NickLin236/pomodoro-timer).

The consumer download is a standalone Windows 10 x64 `Pomodoro.exe`. It embeds icons, version information, and its manifest; state/settings are created in LocalAppData. Users download this one executable and run it directly. The optional ZIP contains installation/uninstallation scripts and documentation.

The permanent latest-release download link will be:

```text
https://github.com/NickLin236/pomodoro-timer/releases/latest/download/Pomodoro.exe
```

That URL works once a successful published release contains `Pomodoro.exe`. It follows GitHub's [latest-release asset URL format](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases).

## Update the source

Upload only the project source, resources, tests, docs, CMake files, and `.github` workflows. Generated binaries are release attachments. `.gitignore` excludes build output and workspace configuration. A source-only ZIP prepared with this project includes the hidden `.github`, `.gitignore`, and `.gitattributes` entries.

The repository already contains the application and release workflows. From its existing working checkout, commit and push the new version's source and documentation:

```powershell
git add .gitignore CMakeLists.txt README.md CHANGELOG.md Docs include src tests resources packaging .github
git commit -m "Fix 1.2.6 release metadata"
git push origin main
```

Keep the existing Git history and `origin` remote. If GitHub authentication is unavailable in the assistant's session, run the commands on your authenticated development machine. [GitHub CLI](https://cli.github.com/) can configure HTTPS Git authentication using `gh auth login` and `gh auth setup-git`.

GitHub runs the CI workflow for the push. It builds with MSVC on Windows x64, runs the automated tests, and keeps build artifacts on the workflow run. Verify the new commit passes before publishing its tag. A successful branch CI run alone does not create a release.

## Publish version 1.2.6

After the version 1.2.6 commit is pushed and its CI checks pass:

If the tag already exists from the failed run, use the recovery commands below instead of creating it again.

```powershell
git tag v1.2.6
git push origin v1.2.6
```

The release workflow verifies executable protections and checks that the tag matches the CMake, resource, and manifest versions, builds the Windows Release executable with the static CRT, and runs the tests. It publishes a GitHub release only after the checks succeed. The release includes `Pomodoro.exe`, `Pomodoro-1.2.6-windows-x64.zip`, `BUILDINFO.json`, and `SHA256SUMS.txt`. `BUILDINFO.json` identifies the exact source commit used for the executable.

Watch **Actions → Release** in the repository for build/test results. After the workflow succeeds, open [release v1.2.6](https://github.com/NickLin236/pomodoro-timer/releases/tag/v1.2.6) and download `Pomodoro.exe`. The top-level README's latest-download link will then point to that release. The version-specific page is available only after publication. A GitHub-hosted Windows build does not replace the real Windows 10 shell/performance checklist in this repository.

Version 1.2.6 adds the saved sound-volume control and aligns release metadata. Adjusted-volume playback is still pending integration; see the [changelog](../CHANGELOG.md) for the current scope. Keep older published releases and their tags fixed.

For later versions, update `project(Pomodoro VERSION ...)` in `CMakeLists.txt`, the file/product versions in `resources/pomodoro.rc`, and the assembly version in `resources/pomodoro.manifest`. Add a matching `## X.Y.Z` section to `CHANGELOG.md`; the release workflow uses that section as its published notes. Commit and push the changes, then push the matching new `vX.Y.Z` tag. Keep published release tags fixed.

## Recover the failed v1.2.6 tag

The original `v1.2.6` tag points to commit `7a29d827fcdc74dad86f8cbae49a2eabeb987a29`, whose CMake version is still 1.2.5. Rerunning that workflow rebuilds the same old commit. Commit and push the corrected metadata first, then wait for branch CI to pass.

Only if `v1.2.6` has never been published, update that failed tag from the corrected commit:

```powershell
git tag -f v1.2.6
git push --force-with-lease=refs/tags/v1.2.6:7a29d827fcdc74dad86f8cbae49a2eabeb987a29 origin refs/tags/v1.2.6
```

The lease rejects the push if someone has changed the remote tag. If a release has already been published, keep its tag fixed and use a new version with matching CMake/resource/manifest/changelog metadata instead.

## Upload an already-built executable manually

If Actions is unavailable, you can attach the prepared `Pomodoro.exe` to a release using the repository's **Releases → Draft a new release** page. Choose a tag identifying the matching source, attach `Pomodoro.exe` and `SHA256SUMS.txt`, then publish. Do not attach the source ZIP as a substitute for the executable.

The local binary produced in this Linux environment is a MinGW Windows x64 cross-build. GitHub Actions produces the intended MSVC Release build. Local structural checks confirm that the cross-build imports only Windows system DLLs, but it has not been executed on Windows here. See [validation](Validation.md).
