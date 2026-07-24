# Release process

## 1. Verify locally

```text
VERIFY_GITHUB_SOURCE.bat
```

This checks the curated source manifest, Java version label, dual-GPU portability markers, forbidden files, personal paths, and public repository metadata.

## 2. Publish the public repository

The source tree may be published publicly now. Run:

```text
PUBLISH_PUBLIC_GITHUB.bat
```

The helper uses GitHub CLI and asks before creating or pushing the repository. Alternatively, create a public repository in the browser and upload the contents of the curated source ZIP.

## 3. Confirm source CI

The **Source verification** workflow must pass on `main`.

## 4. Build and test NVIDIA Windows

Run **Build NVIDIA Windows worker**, download the no-install tester, and run it on an NVIDIA Windows PC. Keep the returned validation ZIP and marker.

## 5. Retest AMD

Build and run all four exactness suites on the AMD worker after shared CUDA/HIP changes.

## 6. Validate GUI integration

Test automatic, AMD-forced, and NVIDIA-forced launchers; start/stop behavior; result saving; run history; and preview generation.

## 7. Create a prerelease tag

The tag must match `VERSION.txt`, for example:

```text
git tag v0.5.0-alpha.1
git push origin v0.5.0-alpha.1
```

The **Publish curated source prerelease** workflow creates a prerelease with the verified source ZIP and checksum. Do not describe NVIDIA Windows binaries as stable until the Windows GPU test passes.

## 8. Universal runtime later

A universal AMD + NVIDIA runtime ZIP should only be published after both workers and all three launch modes pass from a fresh folder.
