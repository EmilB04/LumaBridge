# Release notes and versioning

One file per release: `vMAJOR.MINOR.PATCH.md`, used as the GitHub release text.

LumaBridge follows [semantic versioning](https://semver.org):

| Bump | When | Example |
|---|---|---|
| **Patch** `0.4.0 → 0.4.1` | Bug fixes only: nothing new to learn, nothing changes for settings. | A device that stopped lighting, a crash, a wrong label. |
| **Minor** `0.4.1 → 0.5.0` | New features or visible changes that keep working as before: settings and setups carry over. | Support for a new device or game, new effects, a redesigned page. |
| **Major** `0.x → 1.0.0`, `1.x → 2.0.0` | Breaking changes: settings or setups that no longer carry over, removed features, a changed install. | A new settings format that needs setting up again. |

A minor or major bump resets the numbers after it to 0. While the major version is 0,
LumaBridge is a pre-release: releases are marked as pre-releases on GitHub.
