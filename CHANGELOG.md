# Changelog

## v0.5.0-alpha.1 — 2026-07-24

- Prepared the first public alpha source tree
- Added source verification and Java compilation CI
- Added public issue templates, pull-request guidance, support documentation, and dependency updates
- Added a confirmation-based GitHub CLI publishing helper
- Added a tag-driven curated source prerelease workflow
- Updated GitHub Actions to current major versions
- Kept NVIDIA Windows binaries explicitly prerelease-only until a real Windows GPU smoke test passes
- Removed internal patch-number naming from public packaging and manifests
- Integrated CUDA timing, device-annotation, and Turing shared-memory portability fixes
- Replaced the final direct HIP runtime include with the shared CUDA/HIP compatibility header
- Confirmed all four exactness suites and the full MEGA production pipeline on a Tesla T4

## v0.5.0-preview

- Added first-run setup and environment summary
- Added configurable output directory with restart-safe switching
- Added clearer Java and GPU-worker launch errors
- Added portable launchers and public release foundations
- Added the retro amber GUI and interactive 3D previews
