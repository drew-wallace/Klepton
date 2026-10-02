#!/usr/bin/env python3
"""Acquire pinned Valve client artifacts and inventory them without executing code.

This is the standalone-runtime feasibility gate, not a Steam login implementation.
No SDK package, mapped image, or interface string establishes a running client.
"""
import argparse
import hashlib
import json
import re
import stat
import struct
import os
import subprocess
import time
import shutil
from pathlib import Path, PurePosixPath
from urllib.parse import urlparse
from urllib.request import urlopen
import zipfile

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE.parents[1] / "build/steam-runtime"
DEFAULT_LOCK = HERE / "steam_runtime.lock.json"
CDN = "client-update.akamai.steamstatic.com"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verify(data, size, digest):
    if len(data) != size or sha256(data) != digest:
        raise ValueError("artifact size or SHA-256 does not match the lock")


def fetch_file(url, path, digest, size=None):
    parsed = urlparse(url)
    if parsed.scheme != "https" or parsed.hostname != CDN or parsed.query or parsed.fragment:
        raise ValueError("expected a Valve client CDN HTTPS URL")
    if path.exists():
        data = path.read_bytes()
    else:
        with urlopen(url, timeout=60) as response:
            final = urlparse(response.url)
            if final.scheme != "https" or final.hostname != CDN:
                raise ValueError("unexpected download redirect")
            data = response.read((size if size is not None else 1024 * 1024) + 1)
    verify(data, size if size is not None else len(data), digest)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def archive_path(name):
    # Valve's Linux packages contain Windows-style separators too.
    name = name.replace("\\", "/")
    p = PurePosixPath(name)
    if not name or p.is_absolute() or ".." in p.parts or ":" in name:
        raise ValueError(f"unsafe archive path: {name}")
    return p


def extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    members = []
    with zipfile.ZipFile(archive) as z:
        infos = z.infolist()
        if sum(m.file_size for m in infos) > 1024 * 1024 * 1024:
            raise ValueError("package exceeds the 1 GiB extraction budget")
        seen = set()
        # Validate all paths and links before writing any member.
        for m in infos:
            relative = archive_path(m.filename)
            if str(relative) in seen:
                raise ValueError("duplicate archive member")
            seen.add(str(relative))
            target = destination / relative
            if not target.resolve().is_relative_to(root):
                raise ValueError("archive member escapes destination")
            mode = m.external_attr >> 16
            link = None
            if stat.S_ISLNK(mode):
                link = z.read(m).decode("utf-8").replace("\\", "/")
                if PurePosixPath(link).is_absolute() or ":" in link:
                    raise ValueError("absolute archive symlink")
                if not (target.parent / link).resolve().is_relative_to(root):
                    raise ValueError("archive symlink escapes destination")
            members.append((m, target, link, mode))
        for m, target, link, mode in members:
            if m.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            elif link is None:
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink():
                    raise ValueError("refusing to overwrite a symlink with a file")
                target.write_bytes(z.read(m))
                target.chmod(0o755 if mode & 0o111 else 0o644)
        for m, target, link, mode in members:
            if link is not None:
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink() and target.readlink().as_posix() == link:
                    continue
                if target.exists() or target.is_symlink():
                    raise ValueError("refusing to replace an existing symlink target")
                target.symlink_to(link)


def elf_inventory(data):
    """Read ELF metadata. No host readelf dependency and no guest constructors."""
    if data[:4] != b"\x7fELF":
        return None
    if len(data) < 52 or data[5] != 1:
        raise ValueError("truncated or non-little-endian ELF")
    bits = {1: 32, 2: 64}.get(data[4])
    if bits is None:
        raise ValueError("unknown ELF class")
    kind, machine = struct.unpack_from("<HH", data, 16)
    result = {"bits": bits, "machine": machine,
              "architecture": {3: "i386", 62: "x86_64", 183: "aarch64"}.get(machine, "unknown"),
              "elf_type": {2: "executable", 3: "shared-or-PIE"}.get(kind, str(kind))}
    if bits == 32:
        return result
    if len(data) < 64:
        raise ValueError("truncated ELF64 header")

    def chunk(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            raise ValueError("ELF region outside file")
        return data[offset:offset + size]

    def string(table, offset):
        if offset >= len(table):
            raise ValueError("ELF string outside table")
        end = table.find(b"\0", offset)
        if end < 0:
            raise ValueError("unterminated ELF string")
        return table[offset:end].decode("utf-8", errors="replace")

    phoff, shoff = struct.unpack_from("<QQ", data, 32)
    phsize, phnum, shsize, shnum = struct.unpack_from("<HHHH", data, 54)
    interpreter, tls = None, None
    for i in range(phnum):
        if phsize < 56:
            raise ValueError("invalid ELF program header size")
        p = struct.unpack("<IIQQQQQQ", chunk(phoff + i * phsize, 56))
        if p[0] == 3:
            interpreter = chunk(p[2], p[5]).rstrip(b"\0").decode()
        if p[0] == 7:
            tls = {"file_bytes": p[5], "memory_bytes": p[6], "alignment": p[7]}
    sections = []
    for i in range(shnum):
        if shsize < 64:
            raise ValueError("invalid ELF section header size")
        sections.append(struct.unpack("<IIQQQQIIQQ", chunk(shoff + i * shsize, 64)))
    needed, soname, imports, exports, relocations = [], None, [], [], {}
    for s in sections:
        typ, offset, size, link, entsize = s[1], s[4], s[5], s[6], s[9]
        if typ in (6, 11):
            if link >= len(sections):
                raise ValueError("invalid ELF section link")
            st = sections[link]
            strings = chunk(st[4], st[5])
        if typ == 6:
            if size % 16:
                raise ValueError("invalid ELF dynamic section size")
            for pos in range(0, size, 16):
                tag, value = struct.unpack("<qQ", chunk(offset + pos, 16))
                if tag == 1:
                    needed.append(string(strings, value))
                elif tag == 14:
                    soname = string(strings, value)
        elif typ == 11:
            if entsize != 24 or size % entsize:
                raise ValueError("invalid ELF symbol size")
            for pos in range(0, size, entsize):
                name, info, other, index, value, sym_size = struct.unpack("<IBBHQQ", chunk(offset + pos, 24))
                if not name:
                    continue
                n = string(strings, name)
                if index == 0:
                    imports.append({"name": n, "weak": info >> 4 == 2})
                elif n == "CreateInterface" or n.startswith("Steam_") or n == "JNI_OnLoad":
                    exports.append(n)
        elif typ == 4:
            if entsize != 24 or size % entsize:
                raise ValueError("invalid ELF relocation size")
            for pos in range(0, size, entsize):
                _, info, _ = struct.unpack("<QQq", chunk(offset + pos, 24))
                n = str(info & 0xffffffff)
                relocations[n] = relocations.get(n, 0) + 1
    interfaces = sorted({s.decode() for s in re.findall(
        rb"(?:SteamClient[0-9]{3}|SteamUser[0-9]{3}|CLIENTENGINE_INTERFACE_VERSION[0-9]{3})\x00", data)})
    # Binary string references are research leads, never proof of execution.
    capabilities = {}
    groups = {
        "external_process": {"fork", "vfork", "execve", "execvp", "execvpe", "posix_spawn", "waitpid"},
        "shared_memory": {"shm_open", "shm_unlink", "shmget", "shmat", "memfd_create"},
        "event_sync": {"eventfd", "epoll_create1", "epoll_wait", "epoll_ctl", "syscall", "sem_open"},
        "socket_ipc": {"socket", "socketpair", "connect", "bind", "sendmsg", "recvmsg"},
        "executable_memory": {"mmap", "mmap64", "mprotect"},
        "java": {"JNI_GetCreatedJavaVMs", "JNI_CreateJavaVM"},
    }
    names = {i["name"] for i in imports}
    for group, symbols in groups.items():
        capabilities[group] = sorted(names & symbols)
    result.update(soname=soname, needed=needed, interpreter=interpreter, tls=tls,
                  imports=sorted(imports, key=lambda i: i["name"]), entry_points=sorted(exports),
                  interface_strings=[s.rstrip("\0") for s in interfaces],
                  relocation_types=relocations, imported_capabilities=capabilities,
                  glibc_version_strings=sorted({s.decode() for s in re.findall(rb"GLIBC_[0-9.]+", data)}))
    return result


def manifest_snapshot(lock, lock_dir):
    data = (lock_dir / lock["manifest_snapshot"]).read_bytes()
    verify(data, len(data), lock["manifest_sha256"])
    text = data.decode("utf-8")
    version = re.search(r'"version"\s*"([0-9]+)"', text)
    if not version or version[1] != lock["client_version"]:
        raise ValueError("manifest client version does not match the lock")
    for package in lock["packages"]:
        match = re.search(r'"' + re.escape(package["name"]) + r'"\s*\{([^{}]*)\}', text)
        if not match:
            raise ValueError("package absent from the pinned manifest")
        fields = dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]*)"', match[1]))
        if (fields.get("file") != package["archive"] or fields.get("sha2") != package["sha256"]
                or fields.get("size") != str(package["size"])
                or package["url"] != f"https://{CDN}/" + package["archive"]):
            raise ValueError("package metadata does not match the pinned manifest")
        for name in (package["archive"], package["name"]):
            if len(archive_path(name).parts) != 1:
                raise ValueError("package name must be a single path component")
    return data


def acquire(lock, root, snapshot):
    root.mkdir(parents=True, exist_ok=True)
    # The live CDN manifest moves. Keep the original alongside the lock so
    # reproducing this version never silently selects a newer package set.
    (root / archive_path(lock["manifest_archive"])).write_bytes(snapshot)
    for p in lock["packages"]:
        archive = root / p["archive"]
        fetch_file(p["url"], archive, p["sha256"], p["size"])
        extract(archive, root / "artifacts" / p["name"])
        print(f"verified and extracted {p['name']}", flush=True)


def inventory(lock, root, guest):
    records = []
    for p in lock["packages"]:
        archive = root / p["archive"]
        if not archive.is_file():
            raise ValueError(f"missing pinned archive: {archive}")
        verify(archive.read_bytes(), p["size"], p["sha256"])
        # Read verified archives, not mutable extracted files.
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                data = z.read(member)
                elf = elf_inventory(data)
                if elf:
                    records.append(dict(package=p["name"], path=str(archive_path(member.filename)),
                                        size=len(data), sha256=sha256(data), **elf))
    if guest:
        data = guest.read_bytes()
        elf = elf_inventory(data)
        if not elf:
            raise ValueError("guest library is not ELF")
        records.append(dict(package="supplied-game", path=guest.name, size=len(data), sha256=sha256(data), **elf))
    return {"schema_version": 1, "client_version": lock["client_version"],
            "manifest_sha256": lock["manifest_sha256"], "artifacts": records,
            "runtime_gate": {"status": "unproven", "supported_platform_reference": "not_run",
                             "vision_pro_login": "not_run", "ticket_validation": "not_run",
                             "playfab_login": "not_run"},
            "notes": ["Static imports and interface strings do not prove a usable backend or login ABI.",
                      "Linux/glibc and Android/bionic binaries require different ABI support.",
                      "No binaries, credentials, ticket bytes, or tokens belong in tracked source."]}


def probe(lock, root, binary, construct=False, timeout=15, interfaces=False, backend=False):
    if not binary.is_file():
        raise ValueError("probe binary missing; run make build/steam_probe")
    known = inventory(lock, root, None)["artifacts"]
    candidates = [("android", "bins_androidarm64_linuxarm64", "androidarm64/libsteamclient.so"),
                  ("linux-sdk", "bins_sdk_linuxarm64_linuxarm64", "linuxarm64/steamclient.so")]
    runs = []
    run_id = str(time.time_ns())
    log_dir = root / "runs" / run_id
    log_dir.mkdir(parents=True)
    environment = dict(os.environ, KL_STEAM_OFFLINE="0", KL_STEAM_SKIP_RESTART_CHECK="0",
                       KL_TRACE_STEAM="0", KL_VERBOSE_ALL="0", KL_FULL="0")
    data = root.resolve() / "probe-data"
    data.mkdir(exist_ok=True)
    environment["KL_STEAM_PROBE_DATA"] = str(data)
    environment["KL_STEAM_DATA_ROOT"] = str(data)
    # This gate must inspect the selected ELF, not an unrelated cached translation.
    environment.pop("KL_DYLIB_DIR", None)
    for label, package, relative in candidates:
        record = next((a for a in known if a["package"] == package and a["path"] == relative), None)
        if not record:
            raise ValueError(f"pinned candidate missing: {package}/{relative}")
        library = root / "artifacts" / package / relative
        verify(library.read_bytes(), record["size"], record["sha256"])
        modes = [False, True] if label == "android" and (construct or interfaces or backend) else [False]
        for execute in modes:
            stage = ("backend" if backend else "interfaces" if interfaces else "construct") if execute else "inspect"
            log = log_dir / f"probe-{label}-{stage}.log"
            command = [str(binary.resolve()), str(library.resolve())]
            if execute:
                command.append("--" + stage)
            started = time.monotonic()
            with log.open("wb") as output:
                try:
                    completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                               env=environment, timeout=timeout, cwd=data)
                    code = completed.returncode
                    timed_out = False
                except subprocess.TimeoutExpired:
                    code, timed_out = None, True
            shutil.copyfile(log, root / log.name)  # convenient latest view; history stays immutable
            transcript = log.read_text(errors="replace")
            logged_on = re.search(r"\[steam-probe\] user handle matches: [01]; BLoggedOn: ([01])", transcript)
            observations = {
                "constructors_returned": "[steam-probe] constructors returned;" in transcript,
                "client_interface": "[steam-probe] SteamClient023: present" in transcript,
                "local_user_created": bool(re.search(r"\[steam-probe\] legacy CreateGlobalUser: user=[1-9][0-9]* pipe=[1-9][0-9]*", transcript)),
                "client_pipe_created": bool(re.search(r"\[steam-probe\] CreateSteamPipe: [1-9][0-9]*", transcript)),
                "user_interface": "[steam-probe] SteamUser023: present" in transcript,
                "logged_on": bool(int(logged_on.group(1))) if logged_on else None,
            }
            runs.append({"candidate": label, "stage": stage, "sha256": record["sha256"],
                         "exit_code": code, "timed_out": timed_out,
                         "elapsed_ms": round((time.monotonic() - started) * 1000), "log": str(log.relative_to(root)), "observations": observations})
            print(f"{label} {stage}: {'timeout' if timed_out else code}", flush=True)
    return {"schema_version": 1, "run_id": run_id, "client_version": lock["client_version"],
            "manifest_sha256": lock["manifest_sha256"], "runs": runs,
            "runtime_gate": {"status": "blocked" if any(r["timed_out"] or r["exit_code"] != 4 for r in runs) else "unproven",
                             "supported_platform_reference": "not_run", "vision_pro_login": "not_run",
                             "ticket_validation": "not_run", "playfab_login": "not_run"},
            "note": "A mapped image or successful constructor is not authenticated client readiness."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fetch", "inventory", "probe"))
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--guest", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--probe-binary", type=Path, default=HERE.parents[1] / "build/steam_probe")
    parser.add_argument("--construct", action="store_true", help="execute unproven Android client constructors in a disposable host process")
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--interfaces", action="store_true", help="run constructors and documented SteamClient023 pipe calls")
    parser.add_argument("--backend", action="store_true", help="also invoke the private engine factory; no private methods or login")
    args = parser.parse_args()
    try:
        lock = json.loads(args.lock.read_text())
        snapshot = manifest_snapshot(lock, args.lock.parent)
        if args.command == "fetch":
            acquire(lock, args.root, snapshot)
        elif args.command == "inventory":
            out = args.output or args.root / "inventory.json"
            result = inventory(lock, args.root, args.guest)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(result, indent=2) + "\n")
            print(f"inventoried {len(result['artifacts'])} ELF files -> {out}")
        else:
            if not 1 <= args.timeout <= 120:
                raise ValueError("probe timeout must be 1..120 seconds")
            result = probe(lock, args.root, args.probe_binary, args.construct, args.timeout, args.interfaces, args.backend)
            out = args.output or args.root / "gate.json"
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(result, indent=2) + "\n")
            print(f"runtime gate {result['runtime_gate']['status']} -> {out}")
            return 1
        return 0
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        parser.exit(1, f"steam runtime: {e}\n")


if __name__ == "__main__":
    raise SystemExit(main())
