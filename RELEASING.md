# Releasing

Releases are tag-driven. Push a tag beginning with `v` and GitHub Actions builds the Windows executable and creates the release.

```powershell
git checkout main
git pull
git tag v0.1.0
git push origin v0.1.0
```

The workflow builds `console-video-capture.exe`, creates a GitHub Release, generates release notes and attaches the executable.

CI also runs pre-commit checks on non-draft pull requests and validates `main` with a Windows Release build.
