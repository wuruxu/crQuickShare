#!/usr/bin/python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license.
"""One-shot privileged P2P GO lease. stdin EOF releases only this group's resources.

Invoked by crFiles through pkexec, never install a passwordless interpreter rule.
Requires python3-gi, wpa_supplicant's D-Bus P2P API, iproute2 and dnsmasq.
stdout is a private JSON pipe, not a diagnostic log (it contains credentials).
"""
import argparse
import fcntl
import ipaddress
import json
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import time

from gi.repository import Gio, GLib

WPA = 'fi.w1.wpa_supplicant1'
ROOT = '/fi/w1/wpa_supplicant1'
IFACE = WPA + '.Interface'
P2P = IFACE + '.P2PDevice'
GROUP = WPA + '.Group'
NM = 'org.freedesktop.NetworkManager'
NMROOT = '/org/freedesktop/NetworkManager'
NMDEV = NM + '.Device'
PROPS = 'org.freedesktop.DBus.Properties'


class Lease:
    def __init__(self, interface):
        self.interface = interface
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.context = GLib.MainContext.default()
        self.group_iface = None
        self.group_path = None
        self.group_name = None
        self.group_index = None
        self.group_requested = False
        self.group_finished = False
        self.dhcp = None
        self.address = None
        self.lock = None
        self.subscriptions = []
        self.stage = 'preflight'
        self.cancelled = False
        self.nm_device = None
        self.station_connection = None
        self.next_station_check = 0

    def call(self, dest, path, api, method, args=None):
        return self.bus.call_sync(dest, path, api, method, args, None,
                                 Gio.DBusCallFlags.NONE, 2000, None).unpack()

    def get(self, dest, path, api, prop):
        return self.call(dest, path, PROPS, 'Get', GLib.Variant('(ss)', (api, prop)))[0]

    def pump(self):
        while self.context.pending():
            self.context.iteration(False)

    def alive(self):
        self.pump()
        if self.cancelled or self.group_finished:
            return False
        if select.select([sys.stdin.buffer], [], [], 0)[0]:
            # There is no command protocol: any input/EOF ends the lease.
            return False
        if self.dhcp is not None and self.dhcp.poll() is not None:
            return False
        if self.nm_device and time.monotonic() >= self.next_station_check:
            self.next_station_check = time.monotonic() + 1
            if (self.get(NM, self.nm_device, NMDEV, 'ActiveConnection') != self.station_connection
                    or self.get(NM, self.nm_device, NMDEV, 'State') != 100):
                self.stage = 'station_connection_changed'
                return False
        return True

    def run_ip(self, *args):
        # No shell, no inherited PATH, no credentials in process arguments.
        return subprocess.run([self.ip, *args], check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=3).stdout

    def started(self, _bus, _sender, _path, _api, _signal, parameters, *_unused):
        info = parameters.unpack()[0]
        if not self.group_requested or self.group_iface:
            return
        if info.get('role') != 'GO':
            self.cancelled = True
            return
        self.group_iface = info['interface_object']
        self.group_path = info['group_object']

    def finished(self, _bus, _sender, _path, _api, _signal, parameters, *_unused):
        if parameters.unpack()[0].get('interface_object') == self.group_iface:
            self.group_finished = True

    def prepare(self):
        if os.geteuid() != 0 or not re.fullmatch(r'[a-zA-Z0-9_.-]{1,15}', self.interface):
            raise RuntimeError('invalid invocation')
        self.stage = 'dependencies'
        self.ip = shutil.which('ip', path='/usr/sbin:/usr/bin:/sbin:/bin')
        self.dnsmasq = shutil.which('dnsmasq', path='/usr/sbin:/usr/bin:/sbin:/bin')
        if not self.ip or not self.dnsmasq:
            raise RuntimeError('missing dependency')
        self.stage = 'station_preflight'
        phy_path = '/sys/class/net/' + self.interface + '/phy80211'
        phy = os.path.basename(os.path.realpath(phy_path))
        if not os.path.exists(phy_path) or not re.fullmatch(r'phy[0-9]+', phy):
            raise RuntimeError('not wireless')
        # /run is root-owned. Do not follow an existing symlink or truncate it.
        self.lock = os.open('/run/crquickshare-go-' + phy + '.lock',
                            os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.nm_device = self.call(NM, NMROOT, NM, 'GetDeviceByIpIface',
                                  GLib.Variant('(s)', (self.interface,)))[0]
        self.station_connection = self.get(NM, self.nm_device, NMDEV, 'ActiveConnection')
        if self.station_connection == '/' or self.get(NM, self.nm_device, NMDEV, 'State') != 100:
            raise RuntimeError('an active NM station connection is required')
        # Do not compete with an NM-owned P2P activation, including a group
        # still being negotiated and therefore not visible in supplicant yet.
        for device in self.call(NM, NMROOT, NM, 'GetDevices')[0]:
            if (self.get(NM, device, NMDEV, 'DeviceType') == 30 and
                    40 <= self.get(NM, device, NMDEV, 'State') <= 110):
                raise RuntimeError('NetworkManager P2P activation is busy')
        ap = self.get(NM, self.nm_device, NMDEV + '.Wireless', 'ActiveAccessPoint')
        frequency = self.get(NM, ap, NM + '.AccessPoint', 'Frequency')
        if not 2400 <= frequency <= 5900:
            raise RuntimeError('unsupported station frequency')
        # Keep the station channel; do not force the radio to change channels.
        self.mgmt = self.call(WPA, ROOT, WPA, 'GetInterface',
                              GLib.Variant('(s)', (self.interface,)))[0]
        config = self.get(WPA, self.mgmt, P2P, 'P2PDeviceConfig')
        if config.get('NoGroupIface', False):
            raise RuntimeError('separate group interfaces are required')
        # Reject pre-existing P2P groups on this PHY. Never adopt/delete them.
        for path in self.get(WPA, ROOT, WPA, 'Interfaces'):
            name = self.get(WPA, path, IFACE, 'Ifname')
            if os.path.realpath('/sys/class/net/' + name + '/phy80211') != os.path.realpath(phy_path):
                continue
            try:
                existing = self.get(WPA, path, P2P, 'Group')
            except GLib.Error:
                continue
            if existing != '/':
                raise RuntimeError('P2P group already exists')
        for event, callback in [('GroupStarted', self.started), ('GroupFinished', self.finished)]:
            self.subscriptions.append(self.bus.signal_subscribe(
                WPA, P2P, event, self.mgmt, None, Gio.DBusSignalFlags.NONE, callback))
        self.stage = 'group_add'
        if not self.alive():
            raise RuntimeError('cancelled')
        self.group_requested = True
        self.call(WPA, self.mgmt, P2P, 'GroupAdd', GLib.Variant('(a{sv})', ({
            'persistent': GLib.Variant('b', False),
            'frequency': GLib.Variant('i', frequency)},)))
        deadline = time.monotonic() + 12
        while not self.group_iface and self.alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not self.group_iface or not self.alive():
            raise RuntimeError('group creation failed')
        self.group_name = self.get(WPA, self.group_iface, IFACE, 'Ifname')
        if (self.group_name == self.interface or
                not re.fullmatch(r'[a-zA-Z0-9_.-]{1,15}', self.group_name) or
                os.path.realpath('/sys/class/net/' + self.group_name + '/phy80211') != os.path.realpath(phy_path)):
            raise RuntimeError('unexpected group interface')
        self.group_index = socket.if_nametoindex(self.group_name)
        self.stage = 'address_setup'
        # A group already configured by another manager is not ours to reconfigure.
        addresses = json.loads(self.run_ip('-j', '-4', 'address', 'show', 'dev', self.group_name))
        if any(entry.get('addr_info') for entry in addresses):
            raise RuntimeError('group address already managed')
        routes = json.loads(self.run_ip('-j', '-4', 'route', 'show', 'table', 'all'))
        occupied = []
        for route in routes:
            dst = route.get('dst', 'default')
            if dst != 'default':
                occupied.append(ipaddress.ip_network(dst, strict=False))
        network = next((ipaddress.ip_network('192.168.%d.0/24' % n)
                        for n in range(70, 250)
                        if not any(ipaddress.ip_network('192.168.%d.0/24' % n).overlaps(r)
                                   for r in occupied)), None)
        if network is None:
            raise RuntimeError('no unused subnet')
        gateway = str(network.network_address + 1)
        address = gateway + '/24'
        self.run_ip('link', 'set', 'dev', self.group_name, 'up')
        self.run_ip('address', 'add', address, 'dev', self.group_name)
        self.address = address
        self.stage = 'dhcp_start'
        self.dhcp = subprocess.Popen([
            self.dnsmasq, '--conf-file=/dev/null', '--keep-in-foreground', '--port=0',
            '--bind-interfaces', '--interface=' + self.group_name, '--except-interface=lo',
            '--dhcp-range=' + str(network.network_address + 10) + ',' +
            str(network.network_address + 30) + ',255.255.255.0,10m',
            '--dhcp-option=3', '--dhcp-option=6', '--dhcp-authoritative',
            '--dhcp-leasefile=/dev/null', '--leasefile-ro', '--pid-file=', '--no-hosts', '--no-resolv'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.2)
        if not self.alive():
            raise RuntimeError('DHCP or station unavailable')
        ssid = bytes(self.get(WPA, self.group_path, GROUP, 'SSID')).decode('utf-8', 'strict')
        password = self.get(WPA, self.group_path, GROUP, 'Passphrase')
        frequency = self.get(WPA, self.group_path, GROUP, 'Frequency')
        v6 = json.loads(self.run_ip('-j', '-6', 'address', 'show', 'dev', self.group_name))
        ipv6 = next((a['local'] for entry in v6 for a in entry.get('addr_info', [])
                     if a.get('scope') == 'link' and 'tentative' not in a.get('flags', [])
                     and not a.get('tentative', False)), '')
        self.stage = 'ready'
        return dict(status='ready', interface=self.group_name, gateway=gateway,
                    ssid=ssid, password=password, frequency=frequency, ipv6=ipv6)

    def cleanup(self):
        ok = True
        if self.dhcp is not None:
            try:
                self.dhcp.terminate()
                try:
                    self.dhcp.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.dhcp.kill()
                    self.dhcp.wait(timeout=2)
            except OSError:
                ok = False
        # Deliver a late GroupStarted even if the GroupAdd call timed out.
        if self.group_requested and not self.group_iface:
            end = time.monotonic() + 15
            while time.monotonic() < end and not self.group_iface:
                self.pump()
                time.sleep(0.05)
        if self.address and self.group_name:
            try:
                if socket.if_nametoindex(self.group_name) == self.group_index:
                    self.run_ip('address', 'del', self.address, 'dev', self.group_name)
            except (OSError, subprocess.SubprocessError):
                # Group removal also removes its addresses.
                pass
        if self.group_iface and not self.group_finished:
            try:
                # Check the group object still matches before removing it.
                if self.get(WPA, self.group_iface, P2P, 'Group') == self.group_path:
                    self.call(WPA, self.group_iface, P2P, 'Disconnect')
                    end = time.monotonic() + 3
                    while not self.group_finished and time.monotonic() < end:
                        self.pump()
                        time.sleep(0.05)
                    if not self.group_finished and self.group_name and self.group_index:
                        try:
                            self.group_finished = socket.if_nametoindex(self.group_name) != self.group_index
                        except OSError:
                            self.group_finished = True
                    ok = ok and self.group_finished
            except GLib.Error:
                ok = False
        elif self.group_requested and not self.group_iface:
            ok = False  # An unobserved late group cannot be declared cleaned.
        for subscription in self.subscriptions:
            self.bus.signal_unsubscribe(subscription)
        if self.lock is not None:
            os.close(self.lock)
        return ok


def emit(value):
    try:
        print(json.dumps(value, ensure_ascii=True), flush=True)
    except BrokenPipeError:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True)
    args = parser.parse_args()
    lease = Lease(args.interface)
    def stop(_signum, _frame):
        lease.cancelled = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        emit(lease.prepare())
        while lease.alive():
            time.sleep(0.1)
        if lease.group_finished or (lease.dhcp is not None and lease.dhcp.poll() is not None) or lease.stage != 'ready':
            emit(dict(status='error', stage=lease.stage))
    except Exception:
        # Exception bodies can include D-Bus credentials. Only emit stage names.
        emit(dict(status='error', stage=lease.stage))
    finally:
        try:
            cleaned = lease.cleanup()
        except Exception:
            cleaned = False
        emit(dict(status='closed', success=cleaned))


if __name__ == '__main__':
    main()
