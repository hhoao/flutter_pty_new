# flutter_pty Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `flutter_pty` with the missing production PTY capabilities needed by `flutter_alacritty` (especially foreground-command state for [flutter_alacritty#1](https://github.com/hhoao/flutter_alacritty/issues/1)), without rewriting the library.

**Architecture:** Keep the existing C FFI plugin (`forkpty` on Unix/Android, ConPTY on Windows). Add thin native queries (`master_fd`, `tcgetpgrp`, optional process name) and Dart-facing APIs. `flutter_alacritty`'s `FlutterPtyBackend` then exposes `ValueListenable<bool> isForegroundProcessRunning`. Prefer incremental, testable slices over a full rewrite.

**Tech Stack:** Flutter FFI plugin, C (`src/flutter_pty_unix.c` / `flutter_pty_win.c`), `ffigen`, Dart 3, integration tests under `example/integration_test/`.

**Out of scope (YAGNI):** New Rust/portable-pty backend, SSH/remote PTY, uid/gid spawn, ConPTY DLL packaging, full XON/XOFF auto flow-control (unless Phase D is explicitly pulled in).

---

## File map

| File | Responsibility |
|------|----------------|
| `src/flutter_pty.h` | Native C API surface |
| `src/flutter_pty_unix.c` | Unix/Android PTY + new queries |
| `src/flutter_pty_win.c` | Windows ConPTY stubs/approximations |
| `lib/src/flutter_pty_bindings_generated.dart` | Regenerated via `ffigen` |
| `lib/flutter_pty.dart` | Public Dart API |
| `example/integration_test/flutter_pty_test.dart` | Integration coverage |
| `CHANGELOG.md` / `README.md` / `pubspec.yaml` | Docs + version |
| Downstream (separate PR): `flutter_alacritty/lib/pty/*` | Consume new APIs for issue #1 |

---

## Priority phases

| Phase | Deliverable | Why |
|-------|-------------|-----|
| **A** | Master fd + foreground pgid + `isForegroundProcessRunning` | Unblocks issue #1 |
| **B** | Foreground process name (best-effort) | Tab titles / richer UI |
| **C** | Pixel resize + `getSize` | Parity with node-pty / portable-pty |
| **D** | Reliability polish (write errors, buffer, exit signal split) | Production hardening |
| **E** | Wire into `flutter_alacritty` | End-user feature |

Ship A → E first. B–D can follow in the same release train if A is stable.

**Note:** Task 3’s `shellPgid` fix is likely required on real shells (not optional polish)—Task 2’s `fg != pid` heuristic often fails when the shell’s process group leader id differs from `pid`.

---

### Task 1: Native Unix — expose master fd and foreground pgid

**Files:**
- Modify: `src/flutter_pty.h`
- Modify: `src/flutter_pty_unix.c`
- Modify: `src/flutter_pty_win.c` (stub returns)
- Test: `example/integration_test/flutter_pty_test.dart`

- [x] **Step 1: Add C declarations**

In `src/flutter_pty.h`, append:

```c
FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle);

/** Foreground process group id for the PTY, or -1 on error / unsupported. */
FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle);
```

- [x] **Step 2: Implement on Unix**

In `src/flutter_pty_unix.c`:

```c
FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle)
{
    return handle->ptm;
}

FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle)
{
    pid_t pgid = tcgetpgrp(handle->ptm);
    if (pgid < 0) {
        return -1;
    }
    return (int)pgid;
}
```

Include `<unistd.h>` / `<termios.h>` if not already present (they are).

- [x] **Step 3: Stub on Windows**

In `src/flutter_pty_win.c`:

```c
FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle)
{
    (void)handle;
    return -1; /* no POSIX fd */
}

FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle)
{
    (void)handle;
    return -1; /* ConPTY has no tcgetpgrp; Phase A documents unsupported */
}
```

- [x] **Step 4: Regenerate bindings**

Run:

```bash
cd /home/hhoa/git/hhoa/flutter_pty
dart run ffigen --config ffigen.yaml
```

Expected: `lib/src/flutter_pty_bindings_generated.dart` contains `pty_get_master_fd` and `pty_get_foreground_pgid`.

- [x] **Step 5: Commit**

```bash
git add src/flutter_pty.h src/flutter_pty_unix.c src/flutter_pty_win.c \
  lib/src/flutter_pty_bindings_generated.dart
git commit -m "$(cat <<'EOF'
feat(native): expose master fd and foreground pgid on Unix

EOF
)"
```

---

### Task 2: Dart API — `masterFd`, `foregroundPgid`, polling helper

**Files:**
- Modify: `lib/flutter_pty.dart`
- Test: `example/integration_test/flutter_pty_test.dart`

- [x] **Step 1: Write failing integration test**

Append to `example/integration_test/flutter_pty_test.dart`:

```dart
test('Pty.foregroundPgid differs while foreground command runs', () async {
  if (Platform.isWindows) {
    return; // unsupported in Phase A
  }
  final pty = Pty.start(shell);
  final idle = pty.foregroundPgid;
  expect(idle, isNotNull);
  expect(idle, greaterThan(0));

  // Sleep keeps a child in the foreground process group.
  pty.write('sleep 2\n'.toUtf8());
  await Future<void>.delayed(const Duration(milliseconds: 200));
  final busy = pty.foregroundPgid;
  expect(busy, isNotNull);
  expect(busy, isNot(idle));

  pty.kill();
  await pty.exitCode;
});
```

- [x] **Step 2: Run test to verify it fails**

Run (Linux desktop):

```bash
cd /home/hhoa/git/hhoa/flutter_pty/example
flutter test integration_test/flutter_pty_test.dart \
  --name 'foregroundPgid differs'
```

Expected: FAIL — `foregroundPgid` getter missing / compile error.

- [x] **Step 3: Implement Dart getters + poll stream**

In `lib/flutter_pty.dart`, add:

```dart
  /// POSIX master fd, or `null` on Windows / error.
  int? get masterFd {
    final fd = _bindings.pty_get_master_fd(_handle);
    return fd < 0 ? null : fd;
  }

  /// Foreground process group id, or `null` if unavailable.
  int? get foregroundPgid {
    final pgid = _bindings.pty_get_foreground_pgid(_handle);
    return pgid < 0 ? null : pgid;
  }

  /// True when the foreground process group is not the shell's own group.
  ///
  /// Heuristic: compares [foregroundPgid] to the shell [pid]'s process group.
  /// Returns `null` when the platform cannot answer (e.g. Windows Phase A).
  bool? get isForegroundProcessRunning {
    final fg = foregroundPgid;
    if (fg == null) return null;
    // Shell child from forkpty is session/process-group leader in normal cases.
    return fg != pid;
  }

  /// Polls [isForegroundProcessRunning] every [interval].
  /// Emits only on change. Cancelling the subscription stops the timer.
  Stream<bool> foregroundProcessRunningChanges({
    Duration interval = const Duration(milliseconds: 150),
  }) async* {
    bool? last;
    while (true) {
      final current = isForegroundProcessRunning;
      if (current != null && current != last) {
        last = current;
        yield current;
      }
      await Future<void>.delayed(interval);
    }
  }
```

Document the heuristic caveats in a dartdoc comment (job control off, interactive TUI that stays in shell pgid, Windows unsupported).

- [x] **Step 4: Re-run test**

```bash
cd /home/hhoa/git/hhoa/flutter_pty/example
flutter test integration_test/flutter_pty_test.dart \
  --name 'foregroundPgid differs'
```

Expected: PASS on Linux. On failure, check whether `bash` puts `sleep` in a new pgid (it should with job control); if flaky, use `sleep 5 & wait` alternatives or `stdbuf` — prefer a short C helper only if needed.

- [x] **Step 5: Commit**

```bash
git add lib/flutter_pty.dart example/integration_test/flutter_pty_test.dart
git commit -m "$(cat <<'EOF'
feat: add foreground process detection APIs

EOF
)"
```

---

### Task 3: Harden foreground detection edge cases

**Files:**
- Modify: `lib/flutter_pty.dart`
- Modify: `src/flutter_pty_unix.c` (if needed)
- Test: `example/integration_test/flutter_pty_test.dart`

- [x] **Step 1: Add idle-state test**

```dart
test('Pty.isForegroundProcessRunning is false at idle prompt', () async {
  if (Platform.isWindows) return;
  final pty = Pty.start(shell);
  final collector = OutputCollector(pty);
  await collector.waitForFirstChunk();
  await Future<void>.delayed(const Duration(milliseconds: 100));
  expect(pty.isForegroundProcessRunning, isFalse);
  pty.kill();
});
```

- [x] **Step 2: Fix comparison if shell pgid != pid**

If tests show shell `pid` is not the process-group leader, store `shellPgid` at spawn time:

```c
// after fork success in pty_create:
handle->shell_pgid = getpgid(pid); // parent side
```

Expose `pty_get_shell_pgid` and compare `foregroundPgid != shellPgid` in Dart.

- [x] **Step 3: Document Android behavior**

In `README.md`, add a short "Foreground detection" section:

- Works on Linux / macOS / Android (POSIX `tcgetpgrp`)
- Unsupported on Windows in this release (`null`)
- Heuristic only; toybox `sh` may lack job control → may always report idle

- [x] **Step 4: Commit**

```bash
git add lib/flutter_pty.dart src/flutter_pty_unix.c src/flutter_pty.h \
  lib/src/flutter_pty_bindings_generated.dart README.md \
  example/integration_test/flutter_pty_test.dart
git commit -m "$(cat <<'EOF'
fix: harden foreground detection and document platform limits

EOF
)"
```

---

### Task 4 (Phase B): Foreground process name (best-effort)

**Files:**
- Modify: `src/flutter_pty.h`, `src/flutter_pty_unix.c`, `src/flutter_pty_win.c`
- Modify: `lib/src/flutter_pty_bindings_generated.dart` (via ffigen)
- Modify: `lib/flutter_pty.dart`
- Test: `example/integration_test/flutter_pty_test.dart`

- [ ] **Step 1: Write failing integration test**

```dart
test('Pty.foregroundProcessName contains sleep while sleeping', () async {
  if (!Platform.isLinux && !Platform.isAndroid) return;
  final pty = Pty.start(shell);
  pty.write('sleep 2\n'.toUtf8());
  await Future<void>.delayed(const Duration(milliseconds: 200));
  expect(pty.foregroundProcessName, anyOf(contains('sleep'), isNull));
  // Prefer non-null on Linux; allow null only if /proc layout blocks it.
  if (Platform.isLinux) {
    expect(pty.foregroundProcessName, contains('sleep'));
  }
  pty.kill();
  await pty.exitCode;
});
```

- [ ] **Step 2: Add native declaration + Unix implementation**

In `src/flutter_pty.h`:

```c
/** Writes foreground command name into out (NUL-terminated). Returns length, or -1. */
FFI_PLUGIN_EXPORT int pty_get_foreground_name(
    PtyHandle *handle, char *out, int out_len);
```

In `src/flutter_pty_unix.c` (Linux/Android):

```c
FFI_PLUGIN_EXPORT int pty_get_foreground_name(
    PtyHandle *handle, char *out, int out_len)
{
    if (handle == NULL || out == NULL || out_len < 2) return -1;
    pid_t pgid = tcgetpgrp(handle->ptm);
    if (pgid < 0) return -1;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pgid);
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    if (fgets(out, out_len, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    /* strip trailing newline from /proc/.../comm */
    size_t n = strlen(out);
    if (n > 0 && out[n - 1] == '\n') {
        out[n - 1] = '\0';
        n--;
    }
    return (int)n;
}
```

macOS: return `-1` for Phase B (document); optional follow-up with `proc_name`.

Windows stub: return `-1`.

- [ ] **Step 3: Regenerate bindings**

```bash
cd /home/hhoa/git/hhoa/flutter_pty
dart run ffigen --config ffigen.yaml
```

Expected: `pty_get_foreground_name` appears in generated bindings.

- [ ] **Step 4: Implement Dart getter**

```dart
  /// Best-effort foreground command name (e.g. `sleep`), or null.
  String? get foregroundProcessName {
    const len = 256;
    final buf = calloc<Char>(len);
    try {
      final n = _bindings.pty_get_foreground_name(_handle, buf, len);
      if (n < 0) return null;
      return buf.cast<Utf8>().toDartString();
    } finally {
      calloc.free(buf);
    }
  }
```

Requires existing `package:ffi` `calloc` / `Utf8` imports already used in this file.

- [ ] **Step 5: Run test**

```bash
cd /home/hhoa/git/hhoa/flutter_pty/example
flutter test integration_test/flutter_pty_test.dart \
  --name 'foregroundProcessName contains sleep'
```

Expected: PASS on Linux.

- [ ] **Step 6: Commit**

```bash
git add src/flutter_pty.h src/flutter_pty_unix.c src/flutter_pty_win.c \
  lib/src/flutter_pty_bindings_generated.dart lib/flutter_pty.dart \
  example/integration_test/flutter_pty_test.dart README.md
git commit -m "$(cat <<'EOF'
feat: expose best-effort foreground process name

EOF
)"
```

---

### Task 5 (Phase C): Pixel resize + getSize

**Files:**
- Modify: `src/flutter_pty.h`, `src/flutter_pty_unix.c`, `src/flutter_pty_win.c`
- Modify: `lib/src/flutter_pty_bindings_generated.dart` (via ffigen)
- Modify: `lib/flutter_pty.dart`
- Test: `example/integration_test/flutter_pty_test.dart`

- [ ] **Step 1: Write failing integration test**

```dart
test('Pty.resize with pixels round-trips via size on Unix', () async {
  if (Platform.isWindows) return;
  final pty = Pty.start(shell);
  pty.resize(40, 120, pixelWidth: 960, pixelHeight: 720);
  final s = pty.size;
  expect(s, isNotNull);
  expect(s!.rows, 40);
  expect(s.cols, 120);
  // Kernel may zero pixel fields on some platforms; accept either echoed or zero.
  expect(s.pixelWidth, anyOf(960, 0));
  expect(s.pixelHeight, anyOf(720, 0));
  pty.kill();
  await pty.exitCode;
});
```

- [ ] **Step 2: Extend native API**

```c
FFI_PLUGIN_EXPORT int pty_resize_ex(
    PtyHandle *handle,
    int rows, int cols,
    int pixel_width, int pixel_height);

FFI_PLUGIN_EXPORT int pty_get_size(
    PtyHandle *handle,
    int *rows, int *cols,
    int *pixel_width, int *pixel_height);
```

Unix `pty_resize_ex`: fill `struct winsize` (`ws_row/col/xpixel/ypixel`) + `TIOCSWINSZ`.

Unix `pty_get_size`: `TIOCGWINSZ` into out pointers; return `0` / `-1`.

Keep existing `pty_resize` as wrapper calling `pty_resize_ex(..., 0, 0)` so old callers still work.

Windows: ConPTY resize rows/cols; store last pixel values on handle for `get_size`; pixels ignored by OS.

- [ ] **Step 3: Regenerate bindings**

```bash
cd /home/hhoa/git/hhoa/flutter_pty
dart run ffigen --config ffigen.yaml
```

- [ ] **Step 4: Dart API (backward compatible)**

```dart
  void resize(int rows, int cols, {int pixelWidth = 0, int pixelHeight = 0}) {
    _bindings.pty_resize_ex(_handle, rows, cols, pixelWidth, pixelHeight);
  }

  ({int rows, int cols, int pixelWidth, int pixelHeight})? get size {
    final rows = calloc<Int>();
    final cols = calloc<Int>();
    final pw = calloc<Int>();
    final ph = calloc<Int>();
    try {
      final rc = _bindings.pty_get_size(_handle, rows, cols, pw, ph);
      if (rc != 0) return null;
      return (
        rows: rows.value,
        cols: cols.value,
        pixelWidth: pw.value,
        pixelHeight: ph.value,
      );
    } finally {
      calloc.free(rows);
      calloc.free(cols);
      calloc.free(pw);
      calloc.free(ph);
    }
  }
```

- [ ] **Step 5: Run test**

```bash
cd /home/hhoa/git/hhoa/flutter_pty/example
flutter test integration_test/flutter_pty_test.dart \
  --name 'resize with pixels round-trips'
```

Expected: PASS on Linux.

- [ ] **Step 6: Commit**

```bash
git add src/flutter_pty.h src/flutter_pty_unix.c src/flutter_pty_win.c \
  lib/src/flutter_pty_bindings_generated.dart lib/flutter_pty.dart \
  example/integration_test/flutter_pty_test.dart
git commit -m "$(cat <<'EOF'
feat: support pixel dimensions in resize/getSize

EOF
)"
```

---

### Task 6 (Phase D): Reliability polish

**Files:**
- Modify: `src/flutter_pty_unix.c`, `src/flutter_pty_win.c`, `lib/flutter_pty.dart`

Checklist (each can be its own commit):

- [ ] **Step 1: Surface write errors**

`pty_write` currently ignores `write()` return. Change native to return bytes written / `-1`. Add non-breaking Dart:

```dart
int writeBytes(Uint8List data); // returns written count
void write(Uint8List data) { writeBytes(data); } // keep existing
```

Regenerate ffigen if signature of `pty_write` changes from `void` to `int`.

- [ ] **Step 2: Increase read buffer**

Change `char buffer[1024]` → `16384` in Unix and Windows read loops.

- [ ] **Step 3: Split exit code vs signal (Unix)**

Document existing negative-signal convention clearly; optionally add:

```dart
Future<({int exitCode, int? signal})> get exitStatus;
```

- [ ] **Step 4: Fix `pty_error` on Unix and Windows**

Unix today sets `error_message` in places but `pty_error()` returns `NULL`. Change to:

```c
FFI_PLUGIN_EXPORT char *pty_error(void)
{
    return error_message;
}
```

Also populate `error_message` on Windows failures; document that callers should copy the string immediately.

- [ ] **Step 5: Commit each slice separately**

---

### Task 7 (Phase E): Wire into flutter_alacritty for issue #1

**Files (downstream repo):**
- Modify: `/home/hhoa/git/hhoa/flutter_alacritty/lib/pty/pty_backend.dart`
- Modify: `/home/hhoa/git/hhoa/flutter_alacritty/lib/pty/flutter_pty_backend.dart`
- Modify: `/home/hhoa/git/hhoa/flutter_alacritty/pubspec.yaml` (path or version bump)
- Modify: `/home/hhoa/git/hhoa/flutter_alacritty/docs/library-api.md`
- Test: `/home/hhoa/git/hhoa/flutter_alacritty/test/*` + example app

**Prerequisite:** Tasks 1–3 complete (Phase A). Do **not** wait for B–D.

- [ ] **Step 1: Extend `PtyBackend`**

```dart
abstract class PtyBackend {
  // existing...
  /// `null` when the backend cannot determine foreground state.
  ValueListenable<bool>? get isForegroundProcessRunning => null;
}
```

Default `null` keeps SSH/fake backends working.

- [ ] **Step 2: Implement in `FlutterPtyBackend`**

Use a `ValueNotifier<bool>` updated from `pty.foregroundProcessRunningChanges()`; on `kill()`/`dispose`, **cancel the stream subscription** so the infinite poll loop does not leak, then dispose the notifier.

- [ ] **Step 3: Document host usage** (match issue proposal)

```dart
ValueListenableBuilder<bool>(
  valueListenable: pty.isForegroundProcessRunning!,
  builder: (_, running, __) => TerminalTab(
    title: engine.title.value,
    showProgressIndicator: running,
  ),
);
```

- [ ] **Step 4: Point dependency at local `flutter_pty` until published**

```yaml
flutter_pty:
  path: ../flutter_pty
```

- [ ] **Step 5: Manual verify on Linux + Android**

- Idle prompt → indicator off
- `sleep 5` / `git status` (slow) → indicator on
- Exit command → off

- [ ] **Step 6: Commit in flutter_alacritty**

```bash
git commit -m "$(cat <<'EOF'
feat: expose foreground process running state from PtyBackend

Closes #1
EOF
)"
```

---

### Task 8: Release flutter_pty

**Files:**
- Modify: `pubspec.yaml`, `CHANGELOG.md`, `README.md`

Version policy:

- After Phase A (+ optional E consumer): bump to **`0.5.0`** with A-only changelog.
- After B–D land: bump to **`0.5.x` / `0.6.0`** with those bullets (do not pretend they shipped in the first 0.5.0 if they did not).

- [x] **Step 1: Bump version** for the slice actually shipping

- [x] **Step 2: CHANGELOG entry matching shipped tasks only**

Example for A-only:

```markdown
## 0.5.0
* Add `masterFd`, `foregroundPgid`, `isForegroundProcessRunning`, and
  `foregroundProcessRunningChanges` (Unix/Android; Windows returns null).
```

- [x] **Step 3: Run full verification**

```bash
cd /home/hhoa/git/hhoa/flutter_pty
dart analyze
cd example && flutter test integration_test/flutter_pty_test.dart
```

- [ ] **Step 4: Tag / publish when ready** (only if user asks)

```bash
git tag v0.5.0
# dart pub publish  # only with explicit user request
```

---

## Suggested implementation order

1. Task 1–3 (Phase A) → enough for issue #1  
2. Task 7 (wire flutter_alacritty) early so the feature is usable  
3. Task 8 (0.5.0) if you want a publishable A-only release  
4. Task 4–6 as follow-ups, then another version bump

## Risks / notes

- **Heuristic limits:** no job control ⇒ may never flip; some TUIs stay in shell pgid.
- **Windows:** Phase A returns `null`; a later ConPTY process-tree heuristic is optional and separate.
- **Android:** same Unix code path; validate on device/emulator before calling it done.
- **ffigen:** always regenerate after `flutter_pty.h` changes; do not hand-edit bindings.
- **Existing dirty tree:** `flutter_pty` currently has unrelated Android/example modifications — either commit/stash them before starting, or work on a clean branch.

## Success criteria

- [ ] Linux integration test proves idle vs `sleep` foreground pgid change
- [ ] Dart API documents Windows `null` behavior
- [ ] `flutter_alacritty` can drive a tab progress indicator from `ValueListenable<bool>`
- [ ] No rewrite of spawn/I/O core; Android still uses `/dev/ptmx` path
