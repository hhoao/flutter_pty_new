# flutter_pty_new Rename + Publish CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the package to `flutter_pty_new@1.0.0` (keeping Phase A foreground APIs), add alacritty-style pub.dev publish CI, and point `flutter_alacritty` at the new package with a publish-time existence check.

**Architecture:** Do the rename on the existing `feature/flutter-pty-optimization` worktree (already has Phase A). Add `auto-tag.yml` + `publish.yml` + `PUBLISHING.md` modeled on `flutter_alacritty`. Then update the `feature/pty-foreground-running` alacritty worktree to depend on `flutter_pty_new: ^1.0.0` (no path dep) and gate publish on pub.dev.

**Tech Stack:** Flutter FFI plugin, GitHub Actions OIDC pub.dev publishing, Dart 3.

**Spec:** `docs/superpowers/specs/2026-07-09-flutter-pty-new-rename-publish.md`

**Worktrees:**
- Pty: `/home/hhoa/git/hhoa/flutter_pty/.worktrees/flutter-pty-optimization`
- Alacritty: `/home/hhoa/git/hhoa/flutter_alacritty/.worktrees/pty-foreground`

---

## File map

### flutter_pty_new (pty worktree)

| File | Change |
|------|--------|
| `pubspec.yaml` | name/version/homepage |
| `lib/flutter_pty.dart` → `lib/flutter_pty_new.dart` | rename entrypoint + `_libName` |
| `example/**` | imports, package ids |
| `android/**`, `ios/**`, `macos/**`, `linux/**`, `windows/**` | plugin identifiers |
| `README.md`, `CHANGELOG.md` | branding + 1.0.0 notes |
| `.github/workflows/auto-tag.yml` | create |
| `.github/workflows/publish.yml` | create |
| `PUBLISHING.md` | create |
| git `origin` remote | `hhoao/flutter_pty_new` |

### flutter_alacritty (alacritty worktree)

| File | Change |
|------|--------|
| `pubspec.yaml` / lock | `flutter_pty_new: ^1.0.0` |
| `lib/pty/flutter_pty_backend.dart` + tests/docs | import URI |
| `.github/workflows/publish.yml` | verify `flutter_pty_new` on pub.dev |
| `.github/workflows/release.yml` | same verify in publish job |
| `PUBLISHING.md` | three-package order |

**Do not rename** public Dart types (`Pty`, etc.).

---

### Task 1: Pubspec + Dart package rename

**Files:**
- Modify: `pubspec.yaml`
- Rename: `lib/flutter_pty.dart` → `lib/flutter_pty_new.dart`
- Modify: `example/pubspec.yaml`, `example/integration_test/flutter_pty_test.dart`, `example/lib/main.dart` (imports)
- Modify: any other `package:flutter_pty/` imports under example/

- [ ] **Step 1: Update root pubspec**

```yaml
name: flutter_pty_new
version: 1.0.0
homepage: https://github.com/hhoao/flutter_pty_new
# add if missing:
repository: https://github.com/hhoao/flutter_pty_new
```

- [ ] **Step 2: Rename library file and `_libName`**

```bash
git mv lib/flutter_pty.dart lib/flutter_pty_new.dart
```

In the file, set:

```dart
const _libName = 'flutter_pty_new';
```

Keep class `Pty` and all APIs unchanged. Update internal imports of generated bindings if paths break (they should still be `package:flutter_pty_new/src/...` after pubspec rename).

- [ ] **Step 3: Fix example imports**

Replace:

```dart
import 'package:flutter_pty/flutter_pty.dart';
```

with:

```dart
import 'package:flutter_pty_new/flutter_pty_new.dart';
```

Update `example/pubspec.yaml` dependency name to `flutter_pty_new` (path: `../`).

- [ ] **Step 4: Resolve packages**

```bash
cd /home/hhoa/git/hhoa/flutter_pty/.worktrees/flutter-pty-optimization
flutter pub get
cd example && flutter pub get
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
refactor: rename package to flutter_pty_new 1.0.0

EOF
)"
```

---

### Task 2: Native / plugin identifier rename

**Files:**
- Modify: `android/build.gradle`, `android/settings.gradle`, `android/src/main/AndroidManifest.xml` (if package refs)
- Rename/modify: `ios/flutter_pty.podspec` → `ios/flutter_pty_new.podspec` (and `s.name`)
- Rename/modify: `macos/flutter_pty.podspec` → `macos/flutter_pty_new.podspec`
- Modify: `src/CMakeLists.txt` — rename target `flutter_pty` → `flutter_pty_new`, `OUTPUT_NAME "flutter_pty_new"`
- Modify: `linux/CMakeLists.txt`, `windows/CMakeLists.txt` — `PROJECT_NAME`, `flutter_pty_new_bundled_libraries`, `$<TARGET_FILE:flutter_pty_new>`
- Source filenames (`src/flutter_pty.c`, pod `Classes/flutter_pty.c`) may stay; only produced dylib/plugin IDs must change.
- Modify: example android/ios applicationId / plugin registrant if they hardcode old name

- [ ] **Step 1: Android**

In `android/settings.gradle`: `rootProject.name = 'flutter_pty_new'`  
In `android/build.gradle`: `group` / `namespace` → `io.github.hhoao.flutter_pty_new` (or keep `com.example` only if already published that way — prefer `io.github.hhoao.flutter_pty_new` for a new package).

- [ ] **Step 2: iOS/macOS podspecs**

```bash
git mv ios/flutter_pty.podspec ios/flutter_pty_new.podspec
git mv macos/flutter_pty.podspec macos/flutter_pty_new.podspec
```

Set `s.name = 'flutter_pty_new'` in both. Update any podspec references in example Podfiles if present.

- [ ] **Step 3: CMake (required for FFI load)**

In `src/CMakeLists.txt`:

```cmake
add_library(flutter_pty_new SHARED "flutter_pty.c")
set_target_properties(flutter_pty_new PROPERTIES
  PUBLIC_HEADER flutter_pty.h
  OUTPUT_NAME "flutter_pty_new"
)
target_compile_definitions(flutter_pty_new PUBLIC DART_SHARED_LIB)
# Android 16k page link option: target flutter_pty_new
```

In `linux/CMakeLists.txt` and `windows/CMakeLists.txt`:

```cmake
set(PROJECT_NAME "flutter_pty_new")
set(flutter_pty_new_bundled_libraries
  $<TARGET_FILE:flutter_pty_new>
  PARENT_SCOPE
)
```

Flutter tooling expects the bundled_libraries variable name to match the plugin/package name.

- [ ] **Step 4: Smoke test Linux**

```bash
cd example
flutter test integration_test/flutter_pty_test.dart --name 'foreground|isForeground|Pty works'
```

Expected: PASS (at least foreground trio + basic Pty works).

- [ ] **Step 5: Commit**

```bash
git commit -m "$(cat <<'EOF'
refactor: rename native plugin identifiers to flutter_pty_new

EOF
)"
```

---

### Task 3: Docs + CHANGELOG for 1.0.0

**Files:**
- Modify: `README.md`, `CHANGELOG.md`

- [ ] **Step 1: README**

- Title / badges → `flutter_pty_new`
- Usage import → `package:flutter_pty_new/flutter_pty_new.dart`
- Keep Foreground detection section
- Homepage links → `hhoao/flutter_pty_new`

- [ ] **Step 2: CHANGELOG**

Replace draft `0.5.0` section with:

```markdown
## 1.0.0
* First release under the `flutter_pty_new` package name (fork of flutter_pty).
* Add `masterFd`, `foregroundPgid`, `shellPgid`, `isForegroundProcessRunning`, and
  `foregroundProcessRunningChanges` (Unix/Android; Windows returns null).
* Document foreground-detection platform limits in README.
```

Keep older history below for provenance if desired, or note it applies to pre-fork `flutter_pty`.

- [ ] **Step 3: Commit**

```bash
git commit -m "$(cat <<'EOF'
docs: rebrand README/CHANGELOG for flutter_pty_new 1.0.0

EOF
)"
```

---

### Task 4: Publish CI for flutter_pty_new

**Files:**
- Create: `.github/workflows/auto-tag.yml`
- Create: `.github/workflows/publish.yml`
- Create: `PUBLISHING.md`
- Optionally refresh: `.github/workflows/ci.yml` action versions (non-blocking)

- [ ] **Step 1: Copy/adapt auto-tag from alacritty**

Source: `/home/hhoa/git/hhoa/flutter_alacritty/.github/workflows/auto-tag.yml`  
Adapt to this repo (same version-bump → `v{version}` → dispatch `publish.yml`).

- [ ] **Step 2: Create publish.yml**

Must support both tag push **and** `workflow_dispatch` (auto-tag uses `GITHUB_TOKEN`, which does not trigger `on.push.tags` on other workflows — same reason alacritty dispatches `release.yml`).

```yaml
name: Publish to pub.dev
on:
  push:
    tags: ["v*"]
  workflow_dispatch:
    inputs:
      tag:
        description: "Tag to publish (e.g. v1.0.0). Leave empty to use github.ref."
        required: false
        default: ""
permissions:
  contents: read
jobs:
  publish:
    runs-on: ubuntu-latest
    permissions:
      id-token: write
    steps:
      - uses: actions/checkout@v4
        with:
          ref: ${{ inputs.tag || github.ref }}
      - uses: dart-lang/setup-dart@65eb853c7ba17dde3be364c3d2858773e7144260
      - uses: flutter-actions/setup-flutter@18c66a64fb6f6d3338c63cabbc5cd6da395e7f1d
      - run: dart pub get
      - run: dart pub publish --dry-run
      - run: dart pub publish -f
```

Auto-tag’s dispatch step must call `publish.yml` (not alacritty’s `release.yml`):

```bash
gh workflow run publish.yml --repo "${{ github.repository }}" --ref "$tag"
```

- [ ] **Step 3: Write PUBLISHING.md**

Document OIDC setup for `hhoao/flutter_pty_new`, tag pattern `v{{version}}`, and that this package must publish before `flutter_alacritty`.

- [ ] **Step 4: Local dry-run**

```bash
dart pub get
dart pub publish --dry-run
```

Expected: succeeds or only warns about human OIDC/login (no package-name errors).

- [ ] **Step 5: Point origin remote**

```bash
git remote set-url origin https://github.com/hhoao/flutter_pty_new.git
git remote -v
```

- [ ] **Step 6: Commit**

```bash
git commit -m "$(cat <<'EOF'
ci: add auto-tag and pub.dev OIDC publish workflows

EOF
)"
```

---

### Task 5: Point flutter_alacritty at flutter_pty_new

**Files (alacritty worktree):**
- Modify: `pubspec.yaml`, `pubspec.lock`
- Modify: `lib/pty/flutter_pty_backend.dart`
- Modify: tests/docs importing flutter_pty
- Modify: `README.md` / `docs/library-api.md` mentions

- [ ] **Step 1: Dependency**

Replace path dep with:

```yaml
flutter_pty_new: ^1.0.0
```

For local verification before `1.0.0` is on pub.dev, add an absolute path override (relative `../../flutter_pty/...` from `.worktrees/pty-foreground` is wrong):

```yaml
dependency_overrides:
  rust_lib_flutter_alacritty:
    path: packages/rust_lib_flutter_alacritty
  flutter_pty_new:
    path: /home/hhoa/git/hhoa/flutter_pty/.worktrees/flutter-pty-optimization
```

CI publish already strips all `dependency_overrides`. Keep the flutter_pty_new path override on the feature branch until pub.dev has `1.0.0`; then drop it. Ensure description text mentions `flutter_pty_new`.

- [ ] **Step 2: Update imports**

```dart
import 'package:flutter_pty_new/flutter_pty_new.dart';
```

- [ ] **Step 3: flutter pub get + tests**

```bash
cd /home/hhoa/git/hhoa/flutter_alacritty/.worktrees/pty-foreground
flutter pub get
flutter test --exclude-tags integration
```

- [ ] **Step 4: Commit**

```bash
git commit -m "$(cat <<'EOF'
feat: depend on flutter_pty_new for PTY backend

EOF
)"
```

---

### Task 6: Alacritty publish gate + PUBLISHING.md

**Files:**
- Modify: `.github/workflows/publish.yml`
- Modify: `.github/workflows/release.yml` (publish-pubdev job)
- Modify: `PUBLISHING.md`

- [ ] **Step 1: Add flutter_pty_new version check**

Copy the existing rust_lib python+curl block and adapt:

```python
m = re.search(
    r'^\s*flutter_pty_new:\s*\^?([0-9]+\.[0-9]+\.[0-9]+)',
    text,
    re.M,
)
```

```bash
curl -fsS "https://pub.dev/api/packages/flutter_pty_new/versions/${version}"
```

Add this check in both `publish.yml` and `release.yml`'s `publish-pubdev` job (alongside rust_lib).

- [ ] **Step 2: Update PUBLISHING.md order**

```
1. rust_lib_flutter_alacritty
2. flutter_pty_new
3. flutter_alacritty
```

Add OIDC note that `flutter_pty_new` lives in `hhoao/flutter_pty_new`.

- [ ] **Step 3: Commit**

```bash
git commit -m "$(cat <<'EOF'
ci: require flutter_pty_new on pub.dev before publish

EOF
)"
```

---

### Task 7: Final verification

- [ ] **Step 1: Pty worktree**

```bash
cd /home/hhoa/git/hhoa/flutter_pty/.worktrees/flutter-pty-optimization
dart analyze
dart pub publish --dry-run
cd example && flutter test integration_test/flutter_pty_test.dart --name 'foreground|isForeground'
```

- [ ] **Step 2: Alacritty worktree**

```bash
cd /home/hhoa/git/hhoa/flutter_alacritty/.worktrees/pty-foreground
flutter test --exclude-tags integration
```

- [ ] **Step 3: Confirm no remaining `package:flutter_pty/` imports (except historical CHANGELOG)**

```bash
rg "package:flutter_pty/" -g'*.dart' 
rg "name: flutter_pty$" pubspec.yaml
```

Expected: no matches in active code/pubspec.

---

## Human actions after code lands

1. Enable pub.dev automated publishing for `flutter_pty_new` → repo `hhoao/flutter_pty_new`.
2. Push pty branch / merge to main → auto-tag `v1.0.0` → publish.
3. Then merge/publish alacritty.

## Success criteria

- Package name `flutter_pty_new`, version `1.0.0`
- Phase A foreground tests still pass
- Publish workflows present on pty repo
- Alacritty depends on `^1.0.0` and CI verifies pub.dev
- Remotes point at `hhoao/flutter_pty_new`
