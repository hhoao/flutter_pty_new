# flutter_pty_new Rename + Dual Publish CI Design

**Date:** 2026-07-09  
**Decision:** Approach 1 — full rename on the existing feature branch, first publish as `1.0.0` with Phase A foreground APIs, and mirror `flutter_alacritty` publish CI for both packages.

## Goals

1. Rename the package (and all consumer-facing identifiers) from `flutter_pty` to **`flutter_pty_new`**.
2. GitHub repo is already **`hhoao/flutter_pty_new`**; remotes/docs must match.
3. First pub.dev release: **`flutter_pty_new@1.0.0`** (includes Phase A foreground APIs).
4. Add alacritty-style publish CI to `flutter_pty_new` (`auto-tag` + OIDC `publish`).
5. Update `flutter_alacritty` to depend on `flutter_pty_new: ^1.0.0` and verify that version exists on pub.dev before publishing.

## Non-goals

- Renaming the local filesystem directory `/home/hhoa/git/hhoa/flutter_pty` (optional later).
- Publishing / claiming the upstream pub.dev package `flutter_pty` (owned by TerminalStudio).
- Phase B–D flutter_pty features (process name, pixel resize, reliability polish).
- Changing `flutter_alacritty` desktop binary release packaging beyond the new dependency check.

## Context

| Item | Current | Target |
|------|---------|--------|
| GitHub repo | renamed to `hhoao/flutter_pty_new` | remotes/docs updated |
| pub package | not on pub.dev as `flutter_pty_new` | `1.0.0` |
| Upstream `flutter_pty` on pub.dev | TerminalStudio `0.4.2` | leave alone |
| Feature work | Phase A on `feature/flutter-pty-optimization` ([optimization spec](./2026-07-09-flutter-pty-optimization.md)) | fold into rename + `1.0.0` |
| `flutter_alacritty` dep | `flutter_pty: ^0.4.0` (path WIP on feature branch) | `flutter_pty_new: ^1.0.0` |
| Publish CI | alacritty has it; pty does not | both |

## Design

### A. Package rename (`flutter_pty` → `flutter_pty_new`)

Apply consistently across the pty repo (worktree `feature/flutter-pty-optimization` or successor branch):

1. **`pubspec.yaml`**
   - `name: flutter_pty_new`
   - `version: 1.0.0`
   - `homepage` / repository URLs → `https://github.com/hhoao/flutter_pty_new`

2. **Dart imports / library file**
   - Keep entrypoint as `lib/flutter_pty.dart` *or* rename to `lib/flutter_pty_new.dart`.
   - **Decision:** rename entrypoint to `lib/flutter_pty_new.dart` and update all imports to `package:flutter_pty_new/flutter_pty_new.dart` for consistency. Optionally re-export from a short path later if needed — YAGNI for v1.
   - Native dylib load name in Dart (`_libName`) → `flutter_pty_new`.
   - **Public Dart types stay the same** (`Pty`, etc.). “Consumer-facing identifiers” means package name, import URI, dylib/plugin IDs — not renaming the API classes.

3. **Native / plugin identifiers**
   - Android: `group` / `namespace` / `settings.gradle` rootProject name
   - iOS/macOS: podspec `s.name`
   - Linux/Windows CMake project / library names as required for FFI plugin bundling
   - Example app package ids / plugin registrant references

4. **Docs / CHANGELOG**
   - README title, badges, usage snippets
   - CHANGELOG: `## 1.0.0` documenting rename + foreground APIs (replace prior `0.5.0` draft notes)

5. **Git remote**
   - `origin` → `https://github.com/hhoao/flutter_pty_new.git`
   - Keep `upstream` pointing at TerminalStudio only if still useful; do not publish there

### B. Publish CI for `flutter_pty_new`

Mirror `flutter_alacritty` patterns (simplified — no rust_lib, no desktop artifacts):

| Workflow | Trigger | Behavior |
|----------|---------|----------|
| `auto-tag.yml` | push `main` + `pubspec.yaml` change | if version bumped → create/push `v{version}`, dispatch publish |
| `publish.yml` | push tag `v*` | OIDC → `dart pub get` → dry-run → `dart pub publish -f` |
| `ci.yml` | PR/push | keep existing tests; refresh action versions if cheap |

Add `PUBLISHING.md` with one-time pub.dev OIDC setup for repo `hhoao/flutter_pty_new`, tag pattern `v{{version}}`.

**Human prerequisite (not automatable):** on pub.dev, create/claim `flutter_pty_new` and enable GitHub Actions publishing for `hhoao/flutter_pty_new`.

### C. `flutter_alacritty` consumer + publish gate

1. Dependency: `flutter_pty_new: ^1.0.0` (no absolute path dep on mergeable branch).
2. Code: `import 'package:flutter_pty_new/flutter_pty_new.dart';` (and any docs).
3. Keep Phase E `PtyBackend.isForegroundProcessRunning` work.
4. Update `.github/workflows/publish.yml` and `release.yml` publish job:
   - After (or beside) `rust_lib_flutter_alacritty` existence check, verify:
     `https://pub.dev/api/packages/flutter_pty_new/versions/{version}`
   - Version parsed from `pubspec.yaml` constraint (same style as rust_lib check).
5. Update `PUBLISHING.md` release order:

```
rust_lib_flutter_alacritty → flutter_pty_new → flutter_alacritty
```

### D. Release sequence

1. Land rename + Phase A on `flutter_pty_new` `main` as `1.0.0`.
2. Auto-tag / publish `flutter_pty_new@1.0.0` to pub.dev (after OIDC setup).
3. Land alacritty branch depending on `^1.0.0` + publish-gate update.
4. Bump/publish `flutter_alacritty` when ready (existing auto-tag/publish).

### E. Branch / worktree strategy

- Continue in `flutter_pty` worktree `feature/flutter-pty-optimization` (or rename branch to `feature/flutter-pty-new-1.0.0`).
- Continue in `flutter_alacritty` worktree `feature/pty-foreground-running` (extend: swap path dep → version constraint + rename imports).
- Before mergeable PRs: discard dirty `example/pubspec.lock` churn; no absolute path deps.

## Risks

| Risk | Mitigation |
|------|------------|
| pub.dev name `flutter_pty_new` taken later | Claim ASAP with first publish |
| OIDC not configured → tag publish fails | Document setup; dry-run locally first |
| Missed rename in native build | `flutter pub get` + example build/test on Linux; smoke Android/macOS/Windows plugin load if CI matrix allows |
| alacritty publishes before pty_new on pub.dev | Hard fail in publish workflow |
| Import churn for hosts | Document migration: `flutter_pty` → `flutter_pty_new` |

## Success criteria

- [ ] `dart pub publish --dry-run` succeeds for `flutter_pty_new` `1.0.0`
- [ ] All Dart imports use `package:flutter_pty_new/...`
- [ ] Foreground APIs from Phase A still pass integration tests
- [ ] `flutter_pty_new` has `auto-tag.yml` + `publish.yml`
- [ ] `flutter_alacritty` depends on `flutter_pty_new: ^1.0.0` and publish CI verifies it on pub.dev
- [ ] `PUBLISHING.md` in both repos documents the three-package order
- [ ] Remotes point at `hhoao/flutter_pty_new`

## Open human actions (outside code)

1. Confirm GitHub redirect from old `flutter_pty` URL works (already renamed).
2. Enable pub.dev automated publishing for `flutter_pty_new` → `hhoao/flutter_pty_new`.
3. Approve first tag push / publish when CI is ready.
