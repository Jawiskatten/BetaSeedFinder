# Contributing

BetaSeedFinder is an alpha project with a performance-sensitive exact terrain pipeline.

Before opening a pull request:

- keep generated output, previews, binaries, and local configuration out of Git;
- compile with JDK 17 or newer;
- run `VERIFY_GITHUB_SOURCE.bat`;
- do not change native math, gate thresholds, protocols, or result formats without exactness evidence;
- include benchmark results for performance claims;
- keep UI changes usable at common Windows scaling levels;
- preserve the Jawiskatten creator credit.

Native changes should pass all four exactness suites on the affected backend. CUDA changes should also run the MEGA production pipeline benchmark. Do not describe an untested GPU or Windows packaging path as validated.
