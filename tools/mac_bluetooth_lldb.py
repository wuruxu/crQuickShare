"""LLDB driver for capture_mac_bluetooth.py; never attaches to an existing app."""

from datetime import datetime, timezone
import ctypes
import json
from pathlib import Path
import platform
import time

import lldb


METHODS = (
    '-[IOBluetoothDevice openConnection:]',
    '-[IOBluetoothDevice closeConnection]',
    '-[IOBluetoothDevice peerConnected:error:]',
    '-[IOBluetoothDevice peerDisconnected:withError:]',
    '-[IOBluetoothDevice openL2CAPChannelAsync:withPSM:withConfiguration:delegate:]',
    '-[IOBluetoothDevice openL2CAPChannelSync:withPSM:withConfiguration:delegate:]',
    '-[IOBluetoothL2CAPChannel connectionComplete:status:]',
    '-[IOBluetoothL2CAPChannel _initWithDevice:andClassicPeer:PSM:withServiceUUID:]',
    '-[IOBluetoothL2CAPChannel setDelegate:]',
    '-[IOBluetoothL2CAPChannel closeChannel]',
    '-[IOBluetoothL2CAPChannel dealloc]',
    '-[CBL2CAPChannel dealloc]',
    '-[CBClassicPeer closeL2CAPChannel:]',
    '-[CBClassicManager setConnectionStatusHandler:]',
    '-[CBClassicPeer setConnectL2CAPCallback:]',
    '-[CBClassicPeer setDisconnectL2CAPCallback:]',
    '-[CBClassicPeer connectL2CAPCallback]',
    '-[CBClassicPeer handleL2CAPChannelOpened:]',
    '-[IOBluetoothCoreBluetoothCoordinator addDelegate:]',
    '-[IOBluetoothCoreBluetoothCoordinator removeDelegate:]',
    '-[IOBluetoothCoreBluetoothCoordinator peerForAddressString:]',
    '-[IOBluetoothL2CAPChannel peerL2CAPChannelConnected:error:]',
    '-[IOBluetoothL2CAPChannel setCbChannel:]',
    '-[IOBluetoothL2CAPChannel setupL2CAPChannelForDevice]',
    '-[CrQuickShareSdpDelegate l2capChannelOpenComplete:status:]',
    '-[CrQuickShareSdpDelegate l2capChannelClosed:]',
)
BLOCK_PATTERNS = (
    r'IOBluetoothCoreBluetoothCoordinator addDelegate:\].*block_invoke',
)
# Private names are observations of this OS build, never APIs invoked in the target.
# Resolve offsets from the host runtime each time; do not hardcode an ABI layout.
FIELDS = {
    'IOBluetoothDevice': ('_peer', '_targetDelegate'),
    'CBClassicPeer': ('_internalState', '_connectL2CAPCallback',
                      '_disconnectL2CAPCallback', '_L2CAPChannels', '_identifier'),
    'CBClassicManager': ('_connectionStatusHandler',),
    'CBL2CAPChannel': ('_peer', '_PSM', '_cid', '_serviceUUID'),
    'IOBluetoothL2CAPChannel': ('_classicPeer', '_cbChannel', '_channelDelegate',
                               '_isOpeningL2CAP', 'mChannelPSM'),
    'IOBluetoothCoreBluetoothCoordinator': ('_classicManager', '_delegates'),
}
MAX_HITS = 256
MAX_FRAMES = 24
_session = None


def runtime_layout(target):
    """Inspect local framework metadata only; never instantiate a BT object."""
    arch = target.GetTriple().split('-')[0]
    if not (arch.startswith(platform.machine())
            and target.GetAddressByteSize() == ctypes.sizeof(ctypes.c_void_p)):
        raise RuntimeError('Host/target architectures differ; memory snapshots disabled')
    ctypes.CDLL('/System/Library/Frameworks/IOBluetooth.framework/IOBluetooth')
    objc = ctypes.CDLL('/usr/lib/libobjc.A.dylib')
    for name, result, args in (
        ('objc_getClass', ctypes.c_void_p, [ctypes.c_char_p]),
        ('class_getInstanceVariable', ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
        ('class_getInstanceSize', ctypes.c_size_t, [ctypes.c_void_p]),
        ('ivar_getOffset', ctypes.c_ssize_t, [ctypes.c_void_p]),
        ('ivar_getTypeEncoding', ctypes.c_char_p, [ctypes.c_void_p]),
    ):
        function = getattr(objc, name)
        function.restype = result
        function.argtypes = args
    layout = {}
    for name, fields in FIELDS.items():
        cls = objc.objc_getClass(name.encode())
        if not cls:
            continue
        size = objc.class_getInstanceSize(cls)
        if size > 4096:
            continue
        entries = {}
        for field in fields:
            ivar = objc.class_getInstanceVariable(cls, field.encode())
            if not ivar:
                continue
            encoding = objc.ivar_getTypeEncoding(ivar).decode()
            pointer = encoding.startswith('@')
            width = 8 if pointer else {'q': 8, 'Q': 8, 'i': 4, 'I': 4,
                                       's': 2, 'S': 2, 'B': 1, 'C': 1}.get(encoding)
            offset = objc.ivar_getOffset(ivar)
            if width and 0 <= offset <= size - width:
                entries[field] = {'offset': offset, 'width': width,
                                  'encoding': encoding, 'pointer': pointer}
        layout[name] = entries
    return layout


def object_snapshot(process, classname, address):
    entries = _session['layout'].get(classname, {})
    result = {'class': classname, 'address': hex(address)}
    if not address or not entries:
        return result
    length = max(field['offset'] + field['width'] for field in entries.values())
    error = lldb.SBError()
    data = process.ReadMemory(address, length, error)
    if not error.Success() or len(data) != length:
        result['read_error'] = str(error)
        return result
    byteorder = 'little' if process.GetTarget().GetByteOrder() == lldb.eByteOrderLittle else 'big'
    for name, field in entries.items():
        offset = field['offset']
        number = int.from_bytes(data[offset:offset + field['width']], byteorder,
                                signed=field['encoding'] in ('q', 'i', 's'))
        result[name] = hex(number) if field['pointer'] else number
    return result


def bridge_snapshot(frame):
    if not _session['layout']:
        return {}
    function = frame.GetDisplayFunctionName() or ''
    process = frame.GetThread().GetProcess()
    arm = process.GetTarget().GetTriple().startswith('arm64')
    receiver = frame.FindRegister('x0' if arm else 'rdi').GetValueAsUnsigned()
    argument = frame.FindRegister('x2' if arm else 'rdx').GetValueAsUnsigned()
    objects = {}

    def take(key, cls, address):
        objects[key] = object_snapshot(process, cls, address)
        return objects[key]

    def peer(address):
        take('peer', 'CBClassicPeer', address)

    if function.startswith('-[IOBluetoothDevice '):
        device = take('device', 'IOBluetoothDevice', receiver)
        peer(int(device.get('_peer', '0'), 16))
    elif function.startswith('-[IOBluetoothL2CAPChannel '):
        wrapper = take('wrapper', 'IOBluetoothL2CAPChannel', receiver)
        peer(int(wrapper.get('_classicPeer', '0'), 16))
        native = int(wrapper.get('_cbChannel', '0'), 16)
        if native:
            take('native', 'CBL2CAPChannel', native)
        if 'peerL2CAPChannelConnected:' in function or 'setCbChannel:' in function:
            value = take('argument_native', 'CBL2CAPChannel', argument)
            take('argument_peer', 'CBClassicPeer', int(value.get('_peer', '0'), 16))
    elif function.startswith('-[CBClassicPeer '):
        peer(receiver)
    elif function.startswith('-[CBClassicManager '):
        take('manager', 'CBClassicManager', receiver)
    elif function.startswith('-[CBL2CAPChannel '):
        native = take('native', 'CBL2CAPChannel', receiver)
        peer(int(native.get('_peer', '0'), 16))
    elif function.startswith('-[IOBluetoothCoreBluetoothCoordinator '):
        coordinator = take('coordinator', 'IOBluetoothCoreBluetoothCoordinator', receiver)
        take('manager', 'CBClassicManager', int(coordinator.get('_classicManager', '0'), 16))
    elif function.startswith('-[CrQuickShareSdpDelegate '):
        wrapper = take('callback_wrapper', 'IOBluetoothL2CAPChannel', argument)
        peer(int(wrapper.get('_classicPeer', '0'), 16))
        native = int(wrapper.get('_cbChannel', '0'), 16)
        if native:
            take('native', 'CBL2CAPChannel', native)
    return objects


def watch_block(frame, kind, address):
    if not address:
        return
    process = frame.GetThread().GetProcess()
    error = lldb.SBError()
    # The 64-bit Clang block ABI stores its invoke pointer at byte 16.
    data = process.ReadMemory(address, 24, error)
    if not error.Success() or len(data) != 24:
        record('block_watch_unavailable', block_kind=kind, block=hex(address), reason=str(error))
        return
    raw_invoke = int.from_bytes(data[16:24], 'little')
    invoke = process.FixAddress(raw_invoke, lldb.eAddressMaskTypeCode)
    if invoke in _session['watched_invokes']:
        return
    target = process.GetTarget()
    if not target.ResolveLoadAddress(invoke).GetModule().IsValid():
        record('block_watch_unavailable', block_kind=kind, block=hex(address), invoke=hex(raw_invoke))
        return
    breakpoint = target.BreakpointCreateByAddress(invoke)
    breakpoint.SetScriptCallbackFunction(__name__ + '.on_breakpoint')
    breakpoint.SetAutoContinue(True)
    _session['watched_invokes'].add(invoke)
    _session['block_kinds'][breakpoint.GetID()] = kind
    _session['breakpoints'].append(('block:' + kind, breakpoint))
    record('block_watch_configured', block_kind=kind, block=hex(address), invoke=hex(invoke),
           breakpoint_id=breakpoint.GetID())


def block_snapshot(frame, kind):
    process = frame.GetThread().GetProcess()
    arm = process.GetTarget().GetTriple().startswith('arm64')
    argument = frame.FindRegister('x1' if arm else 'rsi').GetValueAsUnsigned()
    if kind == 'connection_status':
        return {'peer': object_snapshot(process, 'CBClassicPeer', argument)}
    native = object_snapshot(process, 'CBL2CAPChannel', argument)
    return {'native': native,
            'peer': object_snapshot(process, 'CBClassicPeer', int(native.get('_peer', '0'), 16))}


def stack(thread):
    frames = []
    for index in range(min(thread.GetNumFrames(), MAX_FRAMES)):
        frame = thread.GetFrameAtIndex(index)
        module = frame.GetModule().GetFileSpec()
        frames.append({'index': index, 'pc': hex(frame.GetPC()),
                       'function': frame.GetDisplayFunctionName(),
                       'module': str(module)})
    return frames


def record(kind, **fields):
    _session['trace'].write(json.dumps(
        {'event': kind, 'utc': datetime.now(timezone.utc).isoformat(),
         'monotonic_ns': time.monotonic_ns(), **fields}) + '\n')
    _session['trace'].flush()


def on_breakpoint(frame, location, internal_dict):
    # Read registers and symbols only. No expressions, object descriptions,
    # Objective-C messaging, or code injection in the app being diagnosed.
    if _session is None:
        return False
    try:
        _session['hits'] += 1
        thread = frame.GetThread()
        breakpoint_id = location.GetBreakpoint().GetID()
        kind = _session['block_kinds'].get(breakpoint_id)
        registers = {}
        for name in ('x0', 'x1', 'x2', 'x3', 'x4', 'x5',
                     'rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9'):
            value = frame.FindRegister(name)
            if value.IsValid():
                registers[name] = value.GetValue()
        bridge = block_snapshot(frame, kind) if kind else bridge_snapshot(frame)
        record('breakpoint', hit=_session['hits'], watch=kind,
               breakpoint_id=breakpoint_id,
               pid=thread.GetProcess().GetProcessID(), tid=thread.GetThreadID(),
               function=frame.GetDisplayFunctionName(), registers=registers,
               bridge=bridge, frames=stack(thread))
        # Public initializers in the opt-in experiment run with breakpoints
        # suppressed. Recover their installed blocks from later passive reads.
        manager = bridge.get('manager', {})
        watch_block(frame, 'connection_status', int(manager.get('_connectionStatusHandler', '0'), 16))
        peer = bridge.get('peer', {})
        for field, label in (('_connectL2CAPCallback', 'l2cap_connected'),
                             ('_disconnectL2CAPCallback', 'l2cap_disconnected')):
            watch_block(frame, label, int(peer.get(field, '0'), 16))
        function = frame.GetDisplayFunctionName() or ''
        arm = thread.GetProcess().GetTarget().GetTriple().startswith('arm64')
        argument = frame.FindRegister('x2' if arm else 'rdx').GetValueAsUnsigned()
        if function == '-[CBClassicManager setConnectionStatusHandler:]':
            watch_block(frame, 'connection_status', argument)
        elif function == '-[CBClassicPeer setConnectL2CAPCallback:]':
            watch_block(frame, 'l2cap_connected', argument)
        elif function == '-[CBClassicPeer setDisconnectL2CAPCallback:]':
            watch_block(frame, 'l2cap_disconnected', argument)
        if _session['hits'] >= MAX_HITS:
            for label, breakpoint in _session['breakpoints']:
                breakpoint.SetEnabled(False)
            record('hit_limit', limit=MAX_HITS)
    except Exception as error:
        _session['result']['trace_error'] = str(error)
        for label, breakpoint in _session['breakpoints']:
            breakpoint.SetEnabled(False)
    # Auto-continue immediately so a person does not hold up Bluetooth timeouts.
    return False


def public_expression(frame, receiver, selector, expression, returns_int, timeout_seconds=3):
    """Execute a fixed public API expression for the opt-in experiment only."""
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeObjC_plus_plus)
    # LLDB expressions stop at trace breakpoints even with script callbacks.
    # Suppress them only during this call; inspect the resulting objects after
    # returning. Hit counts do not cover the public-call interval.
    options.SetIgnoreBreakpoints(True)
    options.SetUnwindOnError(True)
    options.SetTryAllThreads(False)
    options.SetStopOthers(False)
    options.SetTimeoutInMicroSeconds(timeout_seconds * 1000000)
    started = time.monotonic_ns()
    record('public_api_requested', receiver=receiver if isinstance(receiver, str) else hex(receiver),
           selector=selector, trace_breakpoints_suppressed=True)
    value = frame.EvaluateExpression(expression, options)
    error = value.GetError()
    fields = {'selector': selector, 'elapsed_ms': (time.monotonic_ns() - started) // 1000000,
              'success': error.Success()}
    if error.Success():
        fields['value'] = value.GetValueAsSigned() if returns_int else hex(value.GetValueAsUnsigned())
    else:
        fields['error'] = str(error)
    _session['result']['public_api_calls'].append(fields)
    record('public_api_returned', **fields)
    if not error.Success():
        raise RuntimeError(f'Public API experiment failed: {selector}: {error}')
    return value.GetValueAsSigned() if returns_int else value.GetValueAsUnsigned()


def public_call(frame, receiver, selector, argument=None, returns_int=False,
                timeout_seconds=3, retain_result=False):
    # Explicit signatures are required by the arm64 objc_msgSend ABI. Class and
    # selector strings come exclusively from the fixed public API list below.
    obj = (f'((void *(*)(const char *))objc_getClass)("{receiver}")' if isinstance(receiver, str)
           else f'(void *){receiver}')
    result_type = ('unsigned char' if selector == 'isConnected' else 'int') if returns_int else 'void *'
    signature = 'void *, void *' + (', void *' if argument is not None else '')
    arguments = f'{obj}, ((void *(*)(const char *))sel_registerName)("{selector}")'
    if argument is not None:
        arguments += f', (void *){argument}'
    expression = f'(({result_type} (*)({signature}))objc_msgSend)({arguments})'
    if retain_result:
        # Keep the isolated probe's target alive across debugger expressions and
        # run loop turns. The owned process exits after its single SDP query.
        expression = ('((void *(*)(void *, void *))objc_msgSend)('
                      + expression + ', ((void *(*)(const char *))sel_registerName)("retain"))')
    return public_expression(frame, receiver, selector, expression, returns_int, timeout_seconds)


def public_run_loop(frame, seconds):
    return public_expression(frame, 'CoreFoundation', 'CFRunLoopRunInMode',
        '((int (*)(void *, double, unsigned char))CFRunLoopRunInMode)'
        f'(*(void **)&kCFRunLoopDefaultMode, {seconds}, 0)', True)


def reconnect_snapshot(frame, receiver, stage):
    process = frame.GetThread().GetProcess()
    device = object_snapshot(process, 'IOBluetoothDevice', receiver)
    peer = object_snapshot(process, 'CBClassicPeer', int(device.get('_peer', '0'), 16))
    if '_internalState' not in peer:
        raise RuntimeError('Reconnect verification requires a readable peer state snapshot')
    connected = bool(public_call(frame, receiver, 'isConnected', returns_int=True))
    value = {'stage': stage, 'is_connected': connected, 'device': device, 'peer': peer}
    _session['result']['reconnect_states'].append(value)
    record('public_reconnect_state', **value)
    for field, label in (('_connectL2CAPCallback', 'l2cap_connected'),
                         ('_disconnectL2CAPCallback', 'l2cap_disconnected')):
        watch_block(frame, label, int(peer.get(field, '0'), 16))
    return value


def wait_reconnect_state(frame, receiver, connected, stage):
    deadline = time.monotonic() + 8
    while True:
        state = reconnect_snapshot(frame, receiver, stage)
        if state['is_connected'] == connected and state['peer'].get('_internalState') == (2 if connected else 0):
            return state
        if time.monotonic() >= deadline:
            raise RuntimeError(f'Timed out waiting for public connection state: {stage}')
        public_run_loop(frame, 0.25)


def public_reconnect(frame):
    process = frame.GetThread().GetProcess()
    arm = process.GetTarget().GetTriple().startswith('arm64')
    address = frame.FindRegister('x2' if arm else 'rdx').GetValueAsUnsigned()
    # This stop precedes the SDP deadline and native channel creation. Obtain
    # the same target through the public constructor, then require a real
    # disconnected state before requesting a new baseband connection.
    receiver = public_call(frame, 'IOBluetoothDevice', 'deviceWithAddressString:',
                           address, retain_result=True)
    if not receiver:
        raise RuntimeError('Unable to obtain public reconnect target')
    _session['result']['reconnect_states'] = []
    initial = reconnect_snapshot(frame, receiver, 'initial')
    if not initial['is_connected']:
        status = public_call(frame, receiver, 'openConnection:', 0, True, timeout_seconds=25)
        record('public_reconnect_initial_open', status=status)
        wait_reconnect_state(frame, receiver, True, 'initial_connected')
    status = public_call(frame, receiver, 'closeConnection', returns_int=True, timeout_seconds=5)
    record('public_reconnect_close_returned', status=status)
    # Even a zero close status does not establish physical disconnection. The
    # state transition below must also be correlated with bluetoothd/HCI logs.
    disconnected = False
    try:
        wait_reconnect_state(frame, receiver, False, 'disconnected')
        disconnected = True
    finally:
        # Make one reconnect request even if waiting for the disconnect event
        # fails. Do not leave an intentionally closed device without an attempt
        # to restore it; API/daemon failures remain explicit capture failures.
        status = public_call(frame, receiver, 'openConnection:', 0, True, timeout_seconds=25)
        record('public_reconnect_open_returned', status=status,
               disconnected_state_observed=disconnected)
        wait_reconnect_state(frame, receiver, True, 'reconnected')
        _session['result']['reconnect_connected_confirmed'] = True
    _session['result']['reconnect_transition_observed'] = disconnected


def public_init_step(frame, stage):
    mode = _session['result']['public_init_order']
    if mode == 'reconnect':
        public_reconnect(frame)
    elif stage == 'before_device':
        if mode in ('controller-first', 'all-first', 'settled-first'):
            if not public_call(frame, 'IOBluetoothHostController', 'defaultController'):
                raise RuntimeError('Public controller initialization returned nil')
        if mode in ('inquiry-first', 'all-first', 'settled-first'):
            if not public_call(frame, 'IOBluetoothDeviceInquiry', 'inquiryWithDelegate:', 0):
                raise RuntimeError('Public inquiry initialization returned nil')
        if mode == 'settled-first':
            public_run_loop(frame, 1.0)
    else:
        process = frame.GetThread().GetProcess()
        arm = process.GetTarget().GetTriple().startswith('arm64')
        receiver = frame.FindRegister('x0' if arm else 'rdi').GetValueAsUnsigned()
        record('public_init_device_before', bridge=bridge_snapshot(frame))
        if not public_call(frame, receiver, 'isConnected', returns_int=True):
            # A nil target is synchronous. Restrict this experiment to the
            # already-connected failure; do not block on a new baseband link.
            raise RuntimeError('Connection-first experiment requires an already-connected device')
        public_call(frame, receiver, 'openConnection:', 0, returns_int=True)
        record('public_init_device_after', bridge=bridge_snapshot(frame))
    _session['result']['public_init_completed'].append(stage)
    record('public_init_completed', stage=stage, mode=mode)


def capture(debugger, config_path):
    global _session
    config = json.loads(Path(config_path).read_text(encoding='utf-8'))
    output = Path(config['output'])
    result = {'success': False, 'pid': None, 'exit_status': None,
              'interrupted': False, 'cleanup_confirmed': True}
    mode = config.get('public_init_order')
    if mode:
        result.update(public_init_order=mode, public_api_calls=[], public_init_completed=[])
    process = None
    target = None
    old_async = debugger.GetAsync()
    with (output / 'native-stacks.jsonl').open('w', encoding='utf-8') as trace:
        _session = {'trace': trace, 'hits': 0, 'breakpoints': [], 'result': result, 'layout': {},
                    'watched_invokes': set(), 'block_kinds': {}}
        experiment_breakpoints = {}
        required_stages = set()
        try:
            debugger.SetAsync(True)
            # Function entry preserves Objective-C argument registers.
            command_result = lldb.SBCommandReturnObject()
            debugger.GetCommandInterpreter().HandleCommand(
                'settings set target.skip-prologue false', command_result)
            if not command_result.Succeeded():
                raise RuntimeError(command_result.GetError())
            target = debugger.CreateTarget(config['app'])
            if not target.IsValid():
                raise RuntimeError('Unable to create LLDB target')
            try:
                _session['layout'] = runtime_layout(target)
                record('runtime_layout', macos_version=platform.mac_ver()[0],
                       triple=target.GetTriple(), classes=_session['layout'])
            except Exception as error:
                result['snapshot_error'] = str(error)
                record('runtime_layout_unavailable', reason=str(error))
            for label, regex in [(name, False) for name in METHODS] + [(name, True) for name in BLOCK_PATTERNS]:
                breakpoint = (target.BreakpointCreateByRegex(label) if regex
                              else target.BreakpointCreateByName(label))
                if not breakpoint.IsValid():
                    raise RuntimeError(f'Unable to create breakpoint: {label}')
                breakpoint.SetScriptCallbackFunction(__name__ + '.on_breakpoint')
                breakpoint.SetAutoContinue(True)
                _session['breakpoints'].append((label, breakpoint))
                record('breakpoint_configured', method=label, regex=regex,
                       breakpoint_id=breakpoint.GetID(),
                       locations=breakpoint.GetNumLocations())
            if mode:
                if mode not in ('controller-first', 'inquiry-first', 'connection-first', 'all-first', 'settled-first', 'reconnect'):
                    raise RuntimeError('Unknown public API initialization order')
                if (len(config['app_args']) != 1 or
                        not config['app_args'][0].startswith('--quick-share-mac-sdp-probe=')):
                    raise RuntimeError('Public API experiment requires the isolated SDP probe')
                stages = []
                if mode != 'connection-first':
                    stages.append(('before_device', '+[IOBluetoothDevice deviceWithAddressString:]'))
                if mode in ('connection-first', 'all-first', 'settled-first'):
                    stages.append(('before_channel',
                                   '-[IOBluetoothDevice openL2CAPChannelAsync:withPSM:withConfiguration:delegate:]'))
                for stage, method in stages:
                    required_stages.add(stage)
                    breakpoint = target.BreakpointCreateByName(method)
                    experiment_breakpoints[breakpoint.GetID()] = (stage, breakpoint)
                    _session['breakpoints'].append(('experiment:' + stage, breakpoint))
                record('public_init_configured', mode=mode, public_api_execution=True,
                       pairing_changes=False, disconnect_existing_link=mode == 'reconnect')
            launch = lldb.SBLaunchInfo(config['app_args'])
            launch.SetWorkingDirectory(config['cwd'])
            launch.SetShellExpandArguments(False)
            launch.SetLaunchFlags(launch.GetLaunchFlags() & ~lldb.eLaunchFlagDisableASLR)
            if not (launch.AddOpenFileAction(0, '/dev/null', True, False)
                    and launch.AddOpenFileAction(1, str(output / 'app-stdout.log'), False, True)
                    and launch.AddOpenFileAction(2, str(output / 'app.log'), False, True)):
                raise RuntimeError('Unable to redirect application output')
            listener = lldb.SBListener('crquickshare-bluetooth-capture')
            launch.SetListener(listener)
            error = lldb.SBError()
            process = target.Launch(launch, error)
            if not error.Success() or not process.IsValid():
                raise RuntimeError(f'LLDB launch failed: {error}')
            result['pid'] = process.GetProcessID()
            (output / 'lldb-process.json').write_text(
                json.dumps({'pid': result['pid']}) + '\n', encoding='utf-8')
            record('launched', pid=result['pid'])
            event = lldb.SBEvent()
            while True:
                # LLDB owns native signal handlers. The supervisor requests
                # cooperative shutdown through this file before sending signals.
                if (output / 'lldb-stop').exists():
                    result['interrupted'] = True
                    break
                listener.WaitForEvent(1, event)
                state = process.GetState()
                if state == lldb.eStateExited:
                    result['exit_status'] = process.GetExitStatus()
                    complete = required_stages == set(result.get('public_init_completed', []))
                    if not complete:
                        result['error'] = 'Public API experiment did not reach all initialization points'
                    result['success'] = 'trace_error' not in result and complete
                    break
                if state in (lldb.eStateCrashed, lldb.eStateDetached,
                             lldb.eStateUnloaded, lldb.eStateInvalid):
                    raise RuntimeError(f'Unexpected debuggee state: {state}')
                if state == lldb.eStateStopped:
                    selected = None
                    for thread in process:
                        if thread.GetStopReason() != lldb.eStopReasonBreakpoint:
                            continue
                        for index in range(0, thread.GetStopReasonDataCount(), 2):
                            entry = experiment_breakpoints.get(thread.GetStopReasonDataAtIndex(index))
                            if entry and entry[1].IsEnabled():
                                selected = (thread, entry)
                                break
                        if selected:
                            break
                    if selected:
                        thread, (stage, breakpoint) = selected
                        breakpoint.SetEnabled(False)
                        public_init_step(thread.GetFrameAtIndex(0), stage)
                        error = process.Continue()
                        if not error.Success():
                            raise RuntimeError(f'Unable to resume public API experiment: {error}')
                        continue
                    # Trace breakpoints auto-continue. A crash, signal or other
                    # stop must fail visibly instead of leaving a hidden app.
                    thread = process.GetSelectedThread()
                    reason = thread.GetStopReason()
                    if reason not in (lldb.eStopReasonBreakpoint,
                                      lldb.eStopReasonNone, lldb.eStopReasonInvalid):
                        record('unexpected_stop', reason=reason, frames=stack(thread))
                        raise RuntimeError(f'Unexpected debuggee stop: {reason}')
        except KeyboardInterrupt:
            result['interrupted'] = True
        except Exception as error:
            result['error'] = str(error)
        finally:
            # Only the process created by this driver is eligible for cleanup.
            if process is not None and process.IsValid():
                if process.GetState() not in (lldb.eStateExited, lldb.eStateDetached,
                                              lldb.eStateUnloaded, lldb.eStateInvalid):
                    error = process.Kill()
                    result['cleanup_confirmed'] = error.Success()
                    if not error.Success():
                        result['cleanup_error'] = str(error)
            result['breakpoints'] = [
                {'method': label, 'locations': breakpoint.GetNumLocations(),
                 'hits': breakpoint.GetHitCount()}
                for label, breakpoint in _session['breakpoints']]
            result['hits'] = _session['hits']
            record('finished', **result)
            (output / 'lldb-result.json').write_text(
                json.dumps(result, indent=2) + '\n', encoding='utf-8')
            _session = None
            debugger.SetAsync(old_async)
    if not result['success']:
        print('Bluetooth debugger capture failed or interrupted; see lldb-result.json.')
