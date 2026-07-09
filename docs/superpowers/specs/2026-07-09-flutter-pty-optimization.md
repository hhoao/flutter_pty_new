# flutter_pty Optimization Spec

**Date:** 2026-07-09  
**Repos:** `/home/hhoa/git/hhoa/flutter_pty`  
**Downstream consumer:** `/home/hhoa/git/hhoa/flutter_alacritty`  
**Driver:** [flutter_alacritty#1](https://github.com/hhoao/flutter_alacritty/issues/1)

## Problem

`flutter_pty` covers basic spawn/I/O/resize/kill but lacks APIs that production PTY libraries expose and that `flutter_alacritty` hosts need—especially **whether a foreground command is running** (tab loading indicator).

## Decision

Do **not** rewrite PTY on Rust/`portable-pty`. Incrementally extend the existing C FFI plugin.

## Requirements

### Must (Phase A + E)

1. Expose POSIX master fd (Unix/Android).
2. Expose foreground process group id via `tcgetpgrp`.
3. Dart API: `isForegroundProcessRunning` heuristic + change stream/polling.
4. Windows: return unavailable (`null`), documented—not a blocker for Android/Linux hosts.
5. `flutter_alacritty` `PtyBackend` gains optional `ValueListenable<bool>? isForegroundProcessRunning`.

### Should (Phase B–D)

6. Best-effort foreground process name.
7. Pixel dimensions in resize / getSize.
8. Reliability: larger read buffer, write error surfacing, clearer exit/signal, real `pty_error`.

### Must not

- Replace spawn/I/O core
- Require Termux
- Block release on full Windows foreground parity

## Success criteria

- Linux test: idle vs `sleep` changes foreground pgid
- Host can bind tab progress to the new listenable
- Android continues to use existing `/dev/ptmx` path
