import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_pty/flutter_pty.dart';
import 'package:flutter_test/flutter_test.dart';

String get shell {
  if (Platform.isWindows) {
    return 'cmd.exe';
  }

  if (Platform.isLinux || Platform.isMacOS) {
    return 'bash';
  }

  return 'sh';
}

extension StringToUtf8 on String {
  Uint8List toUtf8() {
    return Uint8List.fromList(
      utf8.encode(this),
    );
  }
}

final nl = Platform.isWindows ? '\r\n' : '\n';

class OutputCollector {
  final Pty pty;

  OutputCollector(this.pty) {
    subscription = pty.output
        .cast<List<int>>()
        .transform(const Utf8Decoder())
        .listen(buffer.write);
  }

  final StringBuffer buffer = StringBuffer();

  late StreamSubscription subscription;

  String get output => buffer.toString();

  late final done = subscription.asFuture();

  Future<void> waitForFirstChunk() async {
    while (buffer.isEmpty) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
  }

  Future<void> waitForOutput(Pattern pattern) async {
    while (pattern.allMatches(output).isEmpty) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
  }
}

void main() {
  test('Pty works', () async {
    final pty = Pty.start(shell);
    pty.write('random input'.toUtf8());

    expect(await pty.output.first, isNotEmpty);

    pty.kill();
  });

  test('Pty.kill works', () async {
    final pty = Pty.start(shell);
    pty.write('random input'.toUtf8());

    pty.kill();
    expect(await pty.exitCode, isNotNull);
  });

  test('Pty.start can set working directory', () async {
    final tempDir = await Directory.systemTemp.createTemp('flutter_pty_test');

    final pty = Pty.start(shell, workingDirectory: tempDir.path);

    if (Platform.isWindows) {
      pty.write('cd$nl'.toUtf8());
    } else {
      pty.write('pwd$nl'.toUtf8());
    }

    final collector = OutputCollector(pty);
    await collector.waitForOutput(tempDir.path);

    pty.kill();
  });

  test('Pty.start can set environment variables', () async {
    final pty = Pty.start(shell, environment: {'TEST_ENV': 'test'});

    if (Platform.isWindows) {
      pty.write('echo %TEST_ENV%$nl'.toUtf8());
    } else {
      pty.write('echo \$TEST_ENV$nl'.toUtf8());
    }

    final collector = OutputCollector(pty);

    await collector.waitForOutput('test');

    pty.kill();
  });

  test('Pty.start can set multiple environment variables', () async {
    final pty = Pty.start(
      shell,
      environment: {
        'TEST_ENV1': 'test1',
        'TEST_ENV2': 'test2',
      },
    );

    if (Platform.isWindows) {
      pty.write('echo %TEST_ENV1% %TEST_ENV2%$nl'.toUtf8());
    } else {
      pty.write('echo \$TEST_ENV1 \$TEST_ENV2$nl'.toUtf8());
    }

    final collector = OutputCollector(pty);

    await collector.waitForOutput('test1 test2');

    pty.kill();
  });

  test('Pty.start can set ack read mode', () async {
    final pty = Pty.start(
      shell,
      ackRead: true,
      environment: {'TEST_ENV': 'some random text'},
    );

    final collector = OutputCollector(pty);
    await collector.waitForFirstChunk();
    expect(collector.output, isNotEmpty);

    if (Platform.isWindows) {
      pty.write('echo %TEST_ENV%$nl'.toUtf8());
    } else {
      pty.write('echo \$TEST_ENV$nl'.toUtf8());
    }

    await Future.delayed(const Duration(milliseconds: 100));
    expect(collector.output.contains('some random text'), isFalse);

    pty.ackRead();
    await Future.delayed(const Duration(milliseconds: 100));
    expect(collector.output.contains('some random text'), isTrue);

    pty.kill();
  });

  test('Pty.foregroundPgid differs while foreground command runs', () async {
    if (Platform.isWindows) {
      return; // unsupported in Phase A
    }
    final pty = Pty.start(shell);
    final collector = OutputCollector(pty);
    await collector.waitForFirstChunk();

    final idle = pty.foregroundPgid;
    expect(idle, isNotNull);
    expect(idle, greaterThan(0));

    // Sleep keeps a child in the foreground process group.
    // Poll briefly: bash job-control setup can take a few hundred ms.
    pty.write('sleep 2\n'.toUtf8());
    int? busy;
    final deadline = DateTime.now().add(const Duration(seconds: 1));
    while (DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      busy = pty.foregroundPgid;
      if (busy != null && busy != idle) break;
    }
    expect(busy, isNotNull);
    expect(busy, isNot(idle));

    // SIGKILL: SIGTERM to the shell can hang while a foreground job is running.
    pty.kill(ProcessSignal.sigkill);
    await pty.exitCode;
  });

  test('Pty.isForegroundProcessRunning is false at idle prompt', () async {
    if (Platform.isWindows) return;
    final pty = Pty.start(shell);
    final collector = OutputCollector(pty);
    await collector.waitForFirstChunk();
    await Future<void>.delayed(const Duration(milliseconds: 100));
    expect(pty.isForegroundProcessRunning, isFalse);
    pty.kill(ProcessSignal.sigkill);
    await pty.exitCode;
  });

  test('Pty.isForegroundProcessRunning is true while sleep runs', () async {
    if (Platform.isWindows) return;
    final pty = Pty.start(shell);
    final collector = OutputCollector(pty);
    await collector.waitForFirstChunk();
    await Future<void>.delayed(const Duration(milliseconds: 100));
    expect(pty.isForegroundProcessRunning, isFalse);

    pty.write('sleep 2\n'.toUtf8());
    bool? busy;
    final deadline = DateTime.now().add(const Duration(seconds: 1));
    while (DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      busy = pty.isForegroundProcessRunning;
      if (busy == true) break;
    }
    expect(busy, isTrue);

    pty.kill(ProcessSignal.sigkill);
    await pty.exitCode;
  });
}
