# Releasing

Releases are created automatically from Git tags that start with `v`.

## Create a release

From an up-to-date `main` branch:

```powershell
git checkout main
git pull
git tag v1.0.0
git push origin v1.0.0
```

The release workflow will:

1. Build `ps2-capture-stream.exe` on `windows-latest` using Visual Studio 2022 x64.
2. Create a GitHub Release for the pushed tag.
3. Generate the release notes automatically from GitHub history.
4. Attach `ps2-capture-stream.exe` to the release.

## Pull request CI

Non-draft pull requests run the repository's pre-commit checks. Draft pull requests are skipped until they are marked ready for review.

## Main branch CI

Every push to `main` runs the pre-commit checks and a clean Windows Release build.
