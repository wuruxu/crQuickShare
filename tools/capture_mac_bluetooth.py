#!/usr/bin/env python3
"""Capture one macOS Quick Share run, optionally including Wi-Fi/Keychain diagnostics."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import plistlib
import signal
import subprocess
import sys
import tempfile
import time


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def resolve_application(path):
    requested = path.expanduser().resolve()
    if requested.is_dir() and requested.suffix == '.app':
        with (requested / 'Contents/Info.plist').open('rb') as stream:
            info = plistlib.load(stream)
        if not isinstance(info, dict) or not isinstance(info.get('CFBundleIdentifier', ''), str):
            raise ValueError('The bundle has invalid application metadata.')
        executable = info.get('CFBundleExecutable')
        if (not isinstance(executable, str) or not executable or
                Path(executable).name != executable or executable in ('.', '..')):
            raise ValueError('The bundle has an invalid CFBundleExecutable.')
        app = requested / 'Contents/MacOS' / executable
        if not app.is_file():
            raise ValueError(f'Bundle executable does not exist: {app}')
        return app, requested, info.get('CFBundleIdentifier', '')
    if not requested.is_file():
        raise ValueError(f'Executable or application bundle does not exist: {requested}')
    return requested, None, ''


def system_predicate(app, bundle_id, network_diagnostics):
    predicate = (
        'process == "bluetoothd" OR process == "bluetoothuserd" OR '
        'process == "BlueTool" OR subsystem CONTAINS[c] "bluetooth" OR '
        'senderImagePath CONTAINS[c] "/IOBluetooth.framework/" OR '
        f'process == {json.dumps(app.name)}'
    )
    if network_diagnostics:
        identity = f'eventMessage CONTAINS[c] {json.dumps(app.name)}'
        if bundle_id:
            identity += f' OR eventMessage CONTAINS[c] {json.dumps(bundle_id)}'
        predicate += (
            ' OR process == "airportd" OR process == "wifid" OR process == "WiFiAgent"'
            ' OR process == "securityd" OR process == "secd"'
            ' OR process CONTAINS[c] "WiFiKeychain"'
            ' OR subsystem CONTAINS[c] "CoreWLAN"'
            ' OR subsystem BEGINSWITH[c] "com.apple.wifi"'
            ' OR senderImagePath CONTAINS[c] "/CoreWLAN.framework/"'
            ' OR eventMessage CONTAINS[c] "WiFiKeychain"'
            f' OR ((subsystem CONTAINS[c] "xpc" OR process == "launchd") AND ({identity}))'
        )
    return predicate


def interrupt_capture(signum, frame):
    raise KeyboardInterrupt


def stop_process(process, debugger_stop=None):
    if process is None or process.poll() is not None:
        return
    if debugger_stop is not None:
        # Let the driver kill/reap its own debuggee before stopping LLDB.
        debugger_stop.touch()
        try:
            process.wait(timeout=12)
            return
        except subprocess.TimeoutExpired:
            pass
    try:
        process.terminate()
    except ProcessLookupError:
        process.wait()
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            process.kill()
        except ProcessLookupError:
            pass
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path,
                        help='Create a new capture directory at this path.')
    parser.add_argument('--lldb', action='store_true',
                        help='Launch under LLDB and capture native channel lifecycle stacks.')
    parser.add_argument('--network-diagnostics', action='store_true',
                        help='Also capture CoreWLAN, Wi-Fi, Keychain and related XPC system logs.')
    parser.add_argument('--public-init-order',
                        choices=('controller-first', 'inquiry-first', 'connection-first', 'all-first', 'settled-first', 'reconnect'),
                        help='Experimental public API ordering; reconnect disconnects the target device. '
                             'Requires --lldb and the isolated SDP probe.')
    parser.add_argument('app', type=Path, help='Path to the crFiles executable or .app bundle.')
    parser.add_argument('app_args', nargs=argparse.REMAINDER,
                        help='Arguments passed to crFiles.')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This collector requires macOS.')
    try:
        app, bundle, bundle_id = resolve_application(args.app)
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        parser.error(str(error))
    if bundle and args.lldb:
        parser.error('For foreground bundle authorization capture, omit --lldb. '
                     'Use an executable path for the separate low-level LLDB probes.')
    if bundle:
        existing = subprocess.run(['/usr/bin/pgrep', '-x', app.name], capture_output=True, text=True)
        if existing.returncode == 0:
            parser.error('Quit the existing crFiles instance before capturing a bundle launch.')
        if existing.returncode != 1:
            parser.error('Cannot check whether crFiles is running: ' + existing.stderr.strip())
    app_args = args.app_args
    if app_args[:1] == ['--']:
        app_args = app_args[1:]
    if args.public_init_order and (not args.lldb or len(app_args) != 1
                                  or not app_args[0].startswith('--quick-share-mac-sdp-probe=')):
        parser.error('--public-init-order requires --lldb and only --quick-share-mac-sdp-probe=ADDRESS.')
    debugger_driver = Path(__file__).resolve().with_name('mac_bluetooth_lldb.py')
    if args.lldb and not debugger_driver.is_file():
        parser.error(f'LLDB driver does not exist: {debugger_driver}')
    if args.output_dir:
        output = args.output_dir.expanduser().resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix='crquickshare-bluetooth-'))

    predicate = system_predicate(app, bundle_id, args.network_diagnostics)
    log_command = ['/usr/bin/log', 'stream', '--style', 'compact',
                   '--level', 'debug', '--predicate', predicate]
    metadata = {'started_utc': utc_now(), 'macos_version': platform.mac_ver()[0],
                'machine': platform.machine(), 'app': str(app),
                'app_modified_ns': app.stat().st_mtime_ns,
                'app_args': app_args, 'log_command': log_command,
                'bundle': str(bundle) if bundle else None,
                'bundle_id': bundle_id, 'network_diagnostics': args.network_diagnostics,
                'lldb': args.lldb, 'public_init_order': args.public_init_order}
    if args.network_diagnostics:
        signing_command = ['/usr/bin/codesign', '-dv', '--verbose=4', str(bundle or app)]
        with (output / 'codesign.log').open('wb') as signing_file:
            signing = subprocess.run(signing_command, stdout=signing_file, stderr=subprocess.STDOUT)
        metadata.update(signing_command=signing_command, signing_returncode=signing.returncode)
    if args.lldb:
        (output / 'app.log').touch(exist_ok=False)
        config_path = output / 'lldb-config.json'
        config_path.write_text(json.dumps(
            {'app': str(app), 'app_args': app_args, 'output': str(output),
             'cwd': str(Path.cwd()), 'public_init_order': args.public_init_order},
            indent=2) + '\n', encoding='utf-8')
        # LLDB gets Python string literals, never shell-expanded command text.
        launch_command = ['/usr/bin/xcrun', 'lldb', '--no-lldbinit', '--batch',
                          '-o', 'script import sys; sys.dont_write_bytecode = True; sys.path.insert(0, '
                          + repr(str(debugger_driver.parent)) + ')',
                          '-o', 'script import mac_bluetooth_lldb; '
                          'mac_bluetooth_lldb.capture(lldb.debugger, '
                          + repr(str(config_path)) + ')']
        metadata['debugger_command'] = launch_command
    elif bundle:
        (output / 'app.log').touch(exist_ok=False)
        (output / 'stdout.log').touch(exist_ok=False)
        launch_command = ['/usr/bin/open', '-n', '-W', '-a', str(bundle),
                          '--stdout', str(output / 'stdout.log'), '--stderr', str(output / 'app.log')]
        if app_args:
            launch_command += ['--args', *app_args]
    else:
        launch_command = [str(app), *app_args]
    metadata['launch_command'] = launch_command
    system_log = None
    application = None
    interrupted = False
    capture_failed = False
    handled_signals = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    old_handlers = {sig: signal.signal(sig, interrupt_capture)
                    for sig in handled_signals}
    print(f'Capture directory: {output}', flush=True)
    print('Close any existing crFiles instance before this run. Reproduce once, '
          'then quit crFiles to finish.', flush=True)
    print('Ctrl+C stops capture and leaves the bundle app running.' if bundle else
          'Ctrl+C stops this captured run.', flush=True)
    if args.network_diagnostics:
        print('System log scope includes Wi-Fi, CoreWLAN, Keychain and related XPC events; '
              'system privacy settings are unchanged.', flush=True)
    if args.public_init_order == 'reconnect':
        print('Reconnect experiment: closes and reopens the target device baseband link. '
              'Other Bluetooth services on that device will be interrupted; pairing is unchanged.', flush=True)
    try:
        with (output / 'system.log').open('wb') as system_file, \
             (output / 'system-stderr.log').open('wb') as system_errors, \
             (output / ('lldb.log' if args.lldb else
                        ('launch-services.log' if bundle else 'app.log'))).open('wb') as app_file:
            system_log = subprocess.Popen(log_command, stdout=system_file,
                                          stderr=system_errors,
                                          start_new_session=True)
            # Fail visibly if the system logger rejects this session, before
            # launching the app. Never elevate or change system logging settings.
            time.sleep(0.5)
            if system_log.poll() is not None:
                raise RuntimeError('System log capture stopped at startup. '
                                   'See system.log and system-stderr.log.')
            application = subprocess.Popen(launch_command, stdout=app_file,
                                           stderr=subprocess.STDOUT,
                                           start_new_session=True)
            metadata['debugger_pid' if args.lldb else
                     ('launcher_pid' if bundle else 'app_pid')] = application.pid
            with (output / 'app.log').open(encoding='utf-8', errors='replace') as live_log:
                while application.poll() is None:
                    if bundle:
                        print(live_log.read(65536), end='', flush=True)
                    if system_log.poll() is not None and not capture_failed:
                        capture_failed = True
                        print('System log capture stopped early. See '
                              'system-stderr.log; app logging continues.',
                              file=sys.stderr, flush=True)
                    time.sleep(0.25)
                if bundle:
                    print(live_log.read(), end='', flush=True)
            # Include daemon messages immediately following native teardown.
            time.sleep(1)
    except KeyboardInterrupt:
        interrupted = True
        if application is not None and application.poll() is None:
            if args.lldb:
                (output / 'lldb-stop').touch()
            else:
                try:
                    application.send_signal(signal.SIGINT)
                except ProcessLookupError:
                    pass
                try:
                    application.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
    except (OSError, RuntimeError) as error:
        capture_failed = True
        metadata['error'] = str(error)
        print(f'Capture failed: {error}', file=sys.stderr)
    finally:
        # Finish bounded child cleanup even if the terminal sends another signal.
        for sig in handled_signals:
            signal.signal(sig, signal.SIG_IGN)
        # Only stop children launched by this collector. In bundle mode the
        # child is open's wait process; the Launch Services app remains running.
        stop_process(application, output / 'lldb-stop' if args.lldb else None)
        if system_log is not None and system_log.poll() is not None:
            capture_failed = True
        stop_process(system_log)
        if bundle and application is not None and application.returncode and not interrupted:
            capture_failed = True
        metadata.update(ended_utc=utc_now(), interrupted=interrupted,
                        capture_failed=capture_failed,
                        log_returncode=system_log.returncode if system_log else None)
        metadata['launcher_returncode' if bundle else 'app_returncode'] = (
            application.returncode if application else None)
        if bundle:
            metadata['app_returncode'] = None  # Launch Services does not expose the app's exit status.
        if args.lldb:
            metadata['debugger_returncode'] = metadata.pop('app_returncode')
            try:
                result = json.loads((output / 'lldb-result.json').read_text(encoding='utf-8'))
                metadata['debugger_result'] = result
                interrupted |= result['interrupted']
                metadata['interrupted'] = interrupted
                metadata['app_pid'] = result['pid']
                metadata['app_returncode'] = result['exit_status']
                metadata['capture_failed'] |= not result['success']
                metadata['capture_failed'] |= metadata['debugger_returncode'] != 0
                metadata['capture_failed'] |= not result['cleanup_confirmed']
            except (OSError, ValueError, KeyError) as error:
                metadata['capture_failed'] = True
                metadata['debugger_error'] = f'Missing or invalid debugger result: {error}'
            capture_failed = metadata['capture_failed']
        (output / 'metadata.json').write_text(
            json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        print(f'Saved app.log, system.log, system-stderr.log and metadata.json '
              f'in {output}', flush=True)
        if args.lldb:
            print('Native stacks: native-stacks.jsonl; debugger status: '
                  'lldb-result.json; debugger output: lldb.log', flush=True)
        if bundle:
            print('Bundle stdout: stdout.log; Launch Services output: launch-services.log; '
                  'launcher_returncode is not the application exit status.', flush=True)
        if args.network_diagnostics:
            print('Signing diagnostics: codesign.log', flush=True)
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
    if interrupted:
        return 130
    if capture_failed:
        return 1
    return metadata['launcher_returncode' if bundle else 'app_returncode']


if __name__ == '__main__':
    sys.exit(main())
