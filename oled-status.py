#!/usr/bin/env python3
"""Foreground Corne OLED status collector and narrowly scoped privileged writer."""

import argparse
import concurrent.futures
import fcntl
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import struct
import subprocess
import sys
import time
import unicodedata


USB_ROOT = Path("/sys/bus/usb/devices")
HID_ROOT = Path("/sys/class/hidraw")
USB_ID = ("8d1d", "343a")
RAW_PREFIX = bytes.fromhex("0660ff0961a101")
PORT_PATTERN = re.compile(r"[0-9]+-[0-9]+(?:\.[0-9]+)*\Z")
MAX_LINE = 1024
COMMAND_TIMEOUT = 0.6
INTERVAL = 1.0


class StatusError(Exception):
    pass


class Stopping(Exception):
    pass


class WriteTimeout(StatusError):
    pass


def stop_requested(_signum, _frame):
    raise Stopping()


def write_expired(_signum, _frame):
    raise WriteTimeout("HID write exceeded one second")


def notice(message):
    print(f"corne-oled: {message}", file=sys.stderr, flush=True)


def command(*args):
    try:
        result = subprocess.run(
            args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=COMMAND_TIMEOUT, check=False,
        )
    except FileNotFoundError:
        raise StatusError(f"{args[0]} is not installed") from None
    except subprocess.TimeoutExpired:
        raise StatusError(f"{args[0]} timed out") from None
    except OSError as exc:
        raise StatusError(f"{args[0]}: {exc}") from exc
    if len(result.stdout) > 16384 or len(result.stderr) > 16384:
        raise StatusError(f"{args[0]} returned oversized output")
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip().replace("\n", " ")[:180]
        raise StatusError(f"{args[0]} exited {result.returncode}: {detail or 'no status available'}")
    return result.stdout.decode("utf-8", "replace").strip()


def ascii_label(text, width):
    # Normalize accents, retain word separation, and never put controls on the wire.
    text = unicodedata.normalize("NFKD", text)
    text = "".join(char for char in text if not unicodedata.combining(char))
    text = "".join(char if 0x20 <= ord(char) <= 0x7E else " " for char in text)
    return " ".join(text.split())[:width]


def audio_status(source):
    output = command("wpctl", "get-volume", source)
    match = re.fullmatch(r"Volume:\s+([0-9]+(?:\.[0-9]+)?)(\s+\[MUTED\])?", output)
    if not match:
        raise StatusError("wpctl returned an unrecognized volume/mute response")
    level = float(match[1])
    if not math.isfinite(level):
        raise StatusError("wpctl returned a nonfinite volume")
    return {"volume": int(min(level, 1.0) * 100 + 0.5), "muted": bool(match[2])}


def collect_microphone():
    return audio_status("@DEFAULT_AUDIO_SOURCE@")["muted"]


def collect_output():
    return audio_status("@DEFAULT_AUDIO_SINK@")


def collect_workspace():
    try:
        workspace = json.loads(command("hyprctl", "activeworkspace", "-j"))
    except json.JSONDecodeError as exc:
        raise StatusError("hyprctl returned invalid JSON") from exc
    if not isinstance(workspace, dict) or not isinstance(workspace.get("name"), str):
        raise StatusError("hyprctl returned no workspace name")
    label = ascii_label(workspace["name"], 8)
    if not label:
        # A numeric workspace ID is still actual workspace information.
        identifier = workspace.get("id")
        if type(identifier) is int and identifier >= 0:
            label = str(identifier)[:8]
        else:
            raise StatusError("workspace name has no printable ASCII representation")
    return label


def collect_media():
    players = command("playerctl", "--list-all").splitlines()
    if not players:
        raise StatusError("no MPRIS media player is available")
    # Pin the selected player for this sample, rather than mixing two players.
    player = players[0]
    state = command("playerctl", "--player", player, "status")
    states = {"Stopped": 0, "Paused": 1, "Playing": 2}
    if state not in states:
        raise StatusError("playerctl returned an unrecognized playback state")
    try:
        label = command("playerctl", "--player", player, "metadata", "--format", "{{artist}} - {{title}}")
        label = ascii_label(label, 16) if label.strip(" -") else ""
    except StatusError:
        # Playback state remains known even if the player has no track metadata.
        label = ""
    return {"state": states[state], "label": label}


COLLECTORS = {
    "microphone": collect_microphone,
    "output": collect_output,
    "workspace": collect_workspace,
    "media": collect_media,
}


def collect(executor, previous_errors):
    futures = {name: executor.submit(provider) for name, provider in COLLECTORS.items()}
    status = {}
    for name, future in futures.items():
        error = None
        try:
            status[name] = future.result()
        except (StatusError, ValueError, OverflowError) as exc:
            status[name] = None
            error = str(exc)
        if previous_errors.get(name) != error:
            notice(f"{name}: unknown ({error})" if error else f"{name}: status available again")
        previous_errors[name] = error
    return status


def exact_keys(value, keys):
    if type(value) is not dict or value.keys() != set(keys):
        raise StatusError("status object has missing or unexpected fields")


def valid_label(value, width, nonempty=False):
    if type(value) is not str or len(value) > width or (nonempty and not value):
        raise StatusError("invalid status label length/type")
    if any(not 0x20 <= ord(char) <= 0x7E for char in value):
        raise StatusError("status labels must contain only printable ASCII")
    return value.encode("ascii").ljust(width, b"\0")


def make_report(status):
    """Validate the entire schema, then construct the only permitted HID message."""
    exact_keys(status, COLLECTORS)
    flags = volume = media_state = 0
    workspace_bytes = b"\0" * 8
    media_bytes = b"\0" * 16
    microphone = status["microphone"]
    if microphone is not None:
        if type(microphone) is not bool:
            raise StatusError("microphone must be a mute boolean or null")
        flags |= 1 | (2 if microphone else 0)
    output = status["output"]
    if output is not None:
        exact_keys(output, ("volume", "muted"))
        if type(output["volume"]) is not int or not 0 <= output["volume"] <= 100:
            raise StatusError("output volume must be an integer from 0 to 100")
        if type(output["muted"]) is not bool:
            raise StatusError("output mute must be a boolean")
        volume = output["volume"]
        flags |= 4 | (8 if output["muted"] else 0)
    workspace = status["workspace"]
    if workspace is not None:
        workspace_bytes = valid_label(workspace, 8, nonempty=True)
        flags |= 16
    media = status["media"]
    if media is not None:
        exact_keys(media, ("state", "label"))
        if type(media["state"]) is not int or not 0 <= media["state"] <= 2:
            raise StatusError("media state must be an integer from 0 to 2")
        media_state = media["state"]
        media_bytes = valid_label(media["label"], 16)
        flags |= 32
    return b"\0OLED" + bytes((1, flags, volume, media_state)) + workspace_bytes + media_bytes


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise StatusError("duplicate JSON field")
        result[key] = value
    return result


def parse_report(line):
    if not line or len(line) > MAX_LINE:
        raise StatusError("empty or oversized status input")
    try:
        status = json.loads(line.decode("ascii"), object_pairs_hook=unique_object)
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        raise StatusError("invalid status JSON") from exc
    return make_report(status)


def usb_identity(port):
    try:
        return tuple((port / field).read_text().strip() for field in ("idVendor", "idProduct", "serial"))
    except OSError:
        return None


def choose_target():
    targets = []
    for port in USB_ROOT.iterdir():
        if not PORT_PATTERN.fullmatch(port.name):
            continue
        identity = usb_identity(port)
        if identity and identity[:2] == USB_ID:
            targets.append((port.name, identity[2]))
    if len(targets) != 1:
        raise StatusError("connect exactly one Corne half in normal keyboard mode before starting")
    port, serial = targets[0]
    if not serial or len(serial) > 256 or not serial.isascii() or not serial.isprintable():
        raise StatusError("Corne has no usable firmware serial; refusing an unpinned target")
    return port, serial


def raw_candidates(port):
    candidates = []
    resolved_port = port.resolve(strict=True)
    for entry in HID_ROOT.glob("hidraw*"):
        try:
            device = (entry / "device").resolve(strict=True)
            if not device.is_relative_to(resolved_port):
                continue
            with (entry / "device/report_descriptor").open("rb") as descriptor:
                if descriptor.read(len(RAW_PREFIX)) != RAW_PREFIX:
                    continue
            candidates.append(entry)
        except (FileNotFoundError, NotADirectoryError):
            continue
    return candidates


def open_target(port_name, serial):
    port = USB_ROOT / port_name
    expected = (*USB_ID, serial)
    if usb_identity(port) != expected:
        raise StatusError("pinned Corne absent or identity changed; waiting for the same USB port and firmware serial")
    candidates = raw_candidates(port)
    if not candidates:
        raise StatusError("pinned Corne has no FF60/61 Raw HID interface; OLED-capable firmware must be flashed first")
    if len(candidates) != 1:
        raise StatusError("multiple FF60/61 interfaces on pinned Corne; refusing an ambiguous target")
    entry = candidates[0]
    node = Path("/dev") / entry.name
    info = node.lstat()
    if not stat.S_ISCHR(info.st_mode):
        raise StatusError("selected hidraw node is not a character device")
    fd = os.open(node, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        opened = os.fstat(fd)
        major, minor = (int(part) for part in (entry / "dev").read_text().strip().split(":"))
        if not stat.S_ISCHR(opened.st_mode) or opened.st_rdev != os.makedev(major, minor):
            raise StatusError("hidraw device changed while opening")
        # Check the open descriptor too; path matching alone has a hotplug race.
        size_buffer = bytearray(4)
        fcntl.ioctl(fd, 0x80044801, size_buffer, True)  # HIDIOCGRDESCSIZE
        size = struct.unpack_from("I", size_buffer)[0]
        if not len(RAW_PREFIX) <= size < 4096:
            raise StatusError("open device has an invalid HID report descriptor size")
        descriptor = bytearray(4100)
        struct.pack_into("I", descriptor, 0, size)
        fcntl.ioctl(fd, 0x90044802, descriptor, True)  # HIDIOCGRDESC
        if descriptor[4:4 + len(RAW_PREFIX)] != RAW_PREFIX:
            raise StatusError("open device does not have QMK Raw HID usage FF60/61")
        if usb_identity(port) != expected or raw_candidates(port) != [entry]:
            raise StatusError("Corne identity/interface changed while opening")
        return fd
    except BaseException:
        os.close(fd)
        raise


def write_report(fd, report):
    # hidraw is report-oriented: never retry a short write as a second report.
    signal.setitimer(signal.ITIMER_REAL, 1.0)
    try:
        if os.write(fd, report) != 33:
            raise StatusError("short HID write")
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)


def writer(port_name, serial):
    # Existing per-user HID ACLs are sufficient; elevation is only a launcher fallback.
    if not PORT_PATTERN.fullmatch(port_name) or not serial or len(serial) > 256 or not serial.isascii() or not serial.isprintable():
        raise StatusError("invalid pinned USB identity")
    signal.signal(signal.SIGALRM, write_expired)
    buffer = bytearray()
    pending = None
    received_at = next_write = 0.0
    fd = None
    last_error = None
    with selectors.DefaultSelector() as selector:
        selector.register(sys.stdin.fileno(), selectors.EVENT_READ)
        try:
            while True:
                now = time.monotonic()
                timeout = max(0.0, next_write - now) if pending is not None else INTERVAL
                if selector.select(min(INTERVAL, timeout)):
                    chunk = os.read(sys.stdin.fileno(), MAX_LINE)
                    if not chunk:
                        if buffer:
                            raise StatusError("collector EOF in the middle of a status record")
                        notice("collector closed; status writer stopped")
                        return 0
                    buffer.extend(chunk)
                    while b"\n" in buffer:
                        line, _, remainder = buffer.partition(b"\n")
                        pending = parse_report(line)
                        buffer = bytearray(remainder)
                        received_at = time.monotonic()
                    if len(buffer) > MAX_LINE:
                        raise StatusError("status input exceeds 1024 bytes per record")
                now = time.monotonic()
                if pending is None or now < next_write:
                    continue
                next_write = now + INTERVAL
                if now - received_at > 3.0:
                    pending = None
                    continue
                try:
                    if fd is None:
                        fd = open_target(port_name, serial)
                    write_report(fd, pending)
                    pending = None
                    if last_error is not False:
                        notice("status report written to the pinned Corne; HID has no receipt/version acknowledgement, so display requires OLED v1 firmware")
                    last_error = False
                except (OSError, StatusError) as exc:
                    if fd is not None:
                        os.close(fd)
                        fd = None
                    message = str(exc)
                    if message != last_error:
                        notice(message)
                    last_error = message
        finally:
            if fd is not None:
                os.close(fd)


def authenticate():
    try:
        with open("/dev/tty", "r+b", buffering=0) as terminal:
            result = subprocess.run(
                ["sudo", "-v"], stdin=terminal, stdout=terminal, stderr=terminal,
                timeout=120, check=False,
            )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise StatusError(f"sudo authentication needs a terminal: {exc}") from exc
    if result.returncode:
        raise StatusError("sudo authentication failed; status stream was not started")


def close_writer(child):
    if child.stdin:
        child.stdin.close()
    try:
        child.wait(timeout=4)
    except subprocess.TimeoutExpired:
        # sudo relays termination to its privileged command; do not kill sudo first.
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            notice("writer has not exited after EOF and SIGTERM; waiting for sudo to reap it")
            child.wait()


def collector(print_only):
    if os.geteuid() == 0:
        raise StatusError("run the collector as your desktop user, not root; only its HID writer uses sudo")
    child = None
    try:
        if not print_only:
            port, serial = choose_target()
            notice(f"selected Corne {serial} on physical USB port {port}; permissions will not be changed")
            writer_command = [sys.executable, str(Path(__file__).resolve()), "--writer", port, serial]
            try:
                probe = open_target(port, serial)
            except PermissionError:
                authenticate()
                writer_command = ["sudo", "-n", "--", *writer_command]
            else:
                os.close(probe)
                notice("using existing user HID access; sudo is not needed")
            child = subprocess.Popen(
                writer_command, stdin=subprocess.PIPE, bufsize=0,
            )
            os.set_blocking(child.stdin.fileno(), False)
        errors = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            while True:
                started = time.monotonic()
                if child is not None and child.poll() is not None:
                    raise StatusError(f"status writer exited with status {child.returncode}")
                status = collect(executor, errors)
                line = (json.dumps(status, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")
                if print_only:
                    print(line.decode("ascii"), end="", flush=True)
                else:
                    try:
                        if os.write(child.stdin.fileno(), line) != len(line):
                            raise StatusError("short status-pipe write")
                    except BlockingIOError:
                        raise StatusError("status writer is not consuming data; stopping rather than buffering stale status") from None
                    except BrokenPipeError:
                        raise StatusError("status writer closed its status pipe") from None
                yield status
                time.sleep(max(0.0, INTERVAL - (time.monotonic() - started)))
    finally:
        if child is not None:
            previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
            try:
                close_writer(child)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)


def main():
    parser = argparse.ArgumentParser(
        description="Stream real Linux desktop status to one Corne OLED, without changing HID permissions.",
        epilog=(
            "Start with exactly one Corne half connected. Default mode samples as your user and uses "
            "existing HID access. If access is denied, it authenticates sudo on /dev/tty for the "
            "status-only writer, never for desktop observation. Requires wpctl (PipeWire/WirePlumber), "
            "hyprctl (Hyprland), playerctl (MPRIS), and sudo only for that fallback; unavailable status "
            "providers become null independently. JSON microphone=true means the default source is muted; "
            "false means open, NOT proof of recording. Media states: 0 stopped, 1 paused, 2 playing. "
            "Labels are ASCII, workspace 8 bytes and media 16 bytes; volume is capped at 100%. "
            "Polls every ~1 second. Ctrl-C stops both processes. Keeps the same USB port and firmware "
            "serial through unplug/flash; moving ports requires restarting. HID writes cannot prove "
            "display receipt: flash OLED v1 firmware first. No autostart or system configuration changes."
        ),
    )
    parser.add_argument("--print", action="store_true", dest="print_only", help="print live status JSON without sudo or HID")
    parser.add_argument("--once", action="store_true", help="collect and print one sample, then exit; implies --print, never touches HID")
    parser.add_argument("--writer", nargs=2, metavar=("USB_PORT", "SERIAL"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    signal.signal(signal.SIGINT, stop_requested)
    signal.signal(signal.SIGTERM, stop_requested)
    try:
        if args.writer:
            if args.print_only or args.once:
                raise StatusError("internal writer cannot be combined with collector options")
            return writer(*args.writer)
        stream = collector(args.print_only or args.once)
        try:
            for _status in stream:
                if args.once:
                    break
        finally:
            stream.close()
        return 0
    except (Stopping, KeyboardInterrupt):
        notice("stopped")
        return 0
    except (StatusError, OSError) as exc:
        notice(str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
