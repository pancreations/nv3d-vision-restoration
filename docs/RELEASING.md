# Building the public release

Use a Windows development environment with the prerequisites in README.md and the RP2040 toolchain described in firmware/rp2040/README.md. From the repository root:

```powershell
py -3 -m pip install --target build/release-python Markdown==3.7
.\tools\Get-Dependencies.ps1
.\tools\Get-CaptureRuntime.ps1
.\tools\Package-Release.ps1
.\tools\Test-ReleaseArchives.ps1 -PublicOnly
```

Microsoft Edge generates both PDFs in headless mode. The README PDF is generated from the current README.md; the quick-start PDF comes from docs/portable-quick-start.html. Packaging builds the app, runs CTest, builds both integration architectures and RP2040 firmware, uses an explicit file allowlist, and verifies an extracted copy before producing the ZIP. `-SkipBuild` is for an already-built and tested tree only.

Publish only the public portable ZIP, the two PDFs, and public SHA256SUMS.txt from dist. Never upload the PRIVATE personal-assets archive or private hash manifest. Mark releases experimental/prerelease while the documented game and optical limitations remain. Commit the matching source and release notes before creating the release tag.
