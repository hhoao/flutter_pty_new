import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter_pty_new/src/flutter_pty_bindings_generated.dart';

const _libName = 'flutter_pty_new';

final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

final _ptyFinalizer = NativeFinalizer(
  _dylib.lookup<NativeFunction<Void Function(Pointer<Void>)>>('pty_close'),
);

final _bindings = FlutterPtyBindings(_dylib);

final _init = () {
  return _bindings.Dart_InitializeApiDL(NativeApi.initializeApiDLData);
}();

void _ensureInitialized() {
  if (_init != 0) {
    throw StateError('Failed to initialize native bindings');
  }
}

/// Pty represents a process running in a pseudo-terminal.
///
/// To create a Pty, use [Pty.start].
class Pty implements Finalizable {
  final String executable;

  final List<String> arguments;

  /// Spawns a process in a pseudo-terminal. The arguments have the same meaning
  /// as in [Process.start].
  /// [ackRead] indicates if the pty should wait for a call to [Pty.ackRead] before sending the next data.
  Pty.start(
    this.executable, {
    this.arguments = const [],
    String? workingDirectory,
    Map<String, String>? environment,
    int rows = 25,
    int columns = 80,
    bool ackRead = false,
  }) {
    _ensureInitialized();

    final effectiveEnv = <String, String>{};

    effectiveEnv['TERM'] = 'xterm-256color';
    // Without this, tools like "vi" produce sequences that are not UTF-8 friendly
    effectiveEnv['LANG'] = 'en_US.UTF-8';

    const envValuesToCopy = {
      'LOGNAME',
      'USER',
      'DISPLAY',
      'LC_TYPE',
      'HOME',
      'PATH'
    };

    for (var entry in Platform.environment.entries) {
      if (envValuesToCopy.contains(entry.key)) {
        effectiveEnv[entry.key] = entry.value;
      }
    }

    if (environment != null) {
      for (var entry in environment.entries) {
        effectiveEnv[entry.key] = entry.value;
      }
    }

    // build argv
    final argv = calloc<Pointer<Utf8>>(arguments.length + 2);
    argv[0] = executable.toNativeUtf8();
    for (var i = 0; i < arguments.length; i++) {
      argv[i + 1] = arguments[i].toNativeUtf8();
    }
    argv[arguments.length + 1] = nullptr;

    //build env
    final envp = calloc<Pointer<Utf8>>(effectiveEnv.length + 1);
    for (var i = 0; i < effectiveEnv.length; i++) {
      final entry = effectiveEnv.entries.elementAt(i);
      envp[i] = '${entry.key}=${entry.value}'.toNativeUtf8();
    }
    envp[effectiveEnv.length] = nullptr;

    final options = calloc<PtyOptions>();
    options.ref.rows = rows;
    options.ref.cols = columns;
    options.ref.executable = executable.toNativeUtf8().cast();
    options.ref.arguments = argv.cast();
    options.ref.environment = envp.cast();
    options.ref.stdout_port = _stdoutPort.sendPort.nativePort;
    options.ref.exit_port = _exitPort.sendPort.nativePort;
    options.ref.ackRead = ackRead;

    if (workingDirectory != null) {
      options.ref.working_directory = workingDirectory.toNativeUtf8().cast();
    } else {
      options.ref.working_directory = nullptr;
    }

    final nativeHandle = _bindings.pty_create(options);

    calloc.free(options.ref.executable);
    if (options.ref.working_directory != nullptr) {
      calloc.free(options.ref.working_directory);
    }
    _freeNativeStringArray(argv, arguments.length + 1);
    _freeNativeStringArray(envp, effectiveEnv.length);
    calloc.free(options);

    _handle = nativeHandle;

    if (_handle == nullptr) {
      throw StateError('Failed to create PTY: ${_getPtyError()}');
    }

    _ptyFinalizer.attach(
      this,
      _handle.cast(),
      detach: this,
      externalSize: 4096,
    );

    _exitPort.first.then(_onExitCode);
  }

  final _stdoutPort = ReceivePort();

  final _exitPort = ReceivePort();

  final _exitCodeCompleter = Completer<int>();

  late final Pointer<PtyHandle> _handle;

  bool _disposed = false;

  /// The output stream from the pseudo-terminal. Note that pseudo-terminals
  /// do not distinguish between stdout and stderr.
  Stream<Uint8List> get output => _stdoutPort.cast();

  /// A `Future` which completes with the exit code of the process
  /// when the process completes.
  ///
  /// The handling of exit codes is platform specific.
  ///
  /// On Linux and OS X a normal exit code will be a positive value in
  /// the range `[0..255]`. If the process was terminated due to a signal
  /// the exit code will be a negative value in the range `[-255..-1]`,
  /// where the absolute value of the exit code is the signal
  /// number. For example, if a process crashes due to a segmentation
  /// violation the exit code will be -11, as the signal SIGSEGV has the
  /// number 11.
  ///
  /// On Windows a process can report any 32-bit value as an exit
  /// code. When returning the exit code this exit code is turned into
  /// a signed value. Some special values are used to report
  /// termination due to some system event. E.g. if a process crashes
  /// due to an access violation the 32-bit exit code is `0xc0000005`,
  /// which will be returned as the negative number `-1073741819`. To
  /// get the original 32-bit value use `(0x100000000 + exitCode) &
  /// 0xffffffff`.
  ///
  /// There is no guarantee that [output] have finished reporting the buffered
  /// output of the process when the returned future completes.
  /// To be sure that all output is captured, wait for the done event on the
  /// streams.
  Future<int> get exitCode => _exitCodeCompleter.future;

  /// The process id of the process running in the pseudo-terminal.
  int get pid {
    _ensureOpen();
    return _bindings.pty_getpid(_handle);
  }

  /// POSIX master fd, or `null` on Windows / error.
  int? get masterFd {
    _ensureOpen();
    final fd = _bindings.pty_get_master_fd(_handle);
    return fd < 0 ? null : fd;
  }

  /// Foreground process group id, or `null` if unavailable.
  int? get foregroundPgid {
    _ensureOpen();
    final pgid = _bindings.pty_get_foreground_pgid(_handle);
    return pgid < 0 ? null : pgid;
  }

  /// Shell process group id captured at spawn, or `null` if unavailable.
  int? get shellPgid {
    _ensureOpen();
    final pgid = _bindings.pty_get_shell_pgid(_handle);
    return pgid < 0 ? null : pgid;
  }

  /// True when the foreground process group is not the shell's own group.
  ///
  /// Compares [foregroundPgid] to [shellPgid] (the process group recorded at
  /// spawn). When a foreground job is running, the PTY's foreground pgid
  /// differs from the shell's.
  ///
  /// Returns `null` when the platform cannot answer (e.g. Windows).
  ///
  /// Caveats: job control off, interactive TUIs that stay in the shell pgid,
  /// and Windows (unsupported) may not report accurately.
  bool? get isForegroundProcessRunning {
    final fg = foregroundPgid;
    final shell = shellPgid;
    if (fg == null || shell == null) return null;
    return fg != shell;
  }

  /// Polls [isForegroundProcessRunning] every [interval].
  /// Emits only on change. Cancelling the subscription stops the timer.
  Stream<bool> foregroundProcessRunningChanges({
    Duration interval = const Duration(milliseconds: 150),
  }) async* {
    bool? last;
    while (!_disposed) {
      final current = isForegroundProcessRunning;
      if (current != null && current != last) {
        last = current;
        yield current;
      }
      await Future<void>.delayed(interval);
    }
  }

  /// Write data to the pseudo-terminal.
  void write(Uint8List data) {
    _ensureOpen();
    final buf = malloc<Int8>(data.length);
    try {
      buf.asTypedList(data.length).setAll(0, data);
      _bindings.pty_write(_handle, buf.cast(), data.length);
    } finally {
      malloc.free(buf);
    }
  }

  /// Resize the pseudo-terminal.
  void resize(int rows, int cols) {
    _ensureOpen();
    _bindings.pty_resize(_handle, rows, cols);
  }

  /// Kill the process running in the pseudo-terminal.
  ///
  /// When possible, [signal] will be sent to the process. This includes
  /// Linux and OS X. The default signal is [ProcessSignal.sigterm]
  /// which will normally terminate the process.
  bool kill([ProcessSignal signal = ProcessSignal.sigterm]) {
    _ensureOpen();
    return Process.killPid(pid, signal);
  }

  /// indicates that a data chunk has been processed.
  /// This is needed when ackRead is set to true as the pty will wait for this signal to happen
  /// before any additional data is sent.
  void ackRead() {
    _ensureOpen();
    _bindings.pty_ack_read(_handle);
  }

  /// Releases native resources associated with this PTY.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _ptyFinalizer.detach(this);
    _bindings.pty_close(_handle);
  }

  void _ensureOpen() {
    if (_disposed) {
      throw StateError('Pty has been disposed');
    }
  }

  void _onExitCode(dynamic exitCode) {
    _stdoutPort.close();
    _exitPort.close();
    _exitCodeCompleter.complete(exitCode);
  }
}

String? _getPtyError() {
  final error = _bindings.pty_error();

  if (error == nullptr) {
    return null;
  }

  return error.cast<Utf8>().toDartString();
}

void _freeNativeStringArray(
  Pointer<Pointer<Utf8>> values,
  int length,
) {
  for (var i = 0; i < length; i++) {
    final value = values[i];
    if (value != nullptr) calloc.free(value);
  }
  calloc.free(values);
}
