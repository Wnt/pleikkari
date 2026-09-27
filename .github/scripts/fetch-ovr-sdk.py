#!/usr/bin/env python3
"""Fetch the private Oculus Mobile SDK bundle for the Go VR CI build (PLE-741).

The Oculus Mobile SDK is licensed and never enters this public repo. Its CI
subset lives in the private workspace repo as a Git LFS object that no commit
references. This fetches it with that repo's read-only deploy key: the key gets
a short-lived LFS download token over SSH (git-lfs-authenticate), which is all a
deploy key can reach. It checks the bundle against the sha256 and size pinned in
build-android.yml, unpacks it, and checks its SHA256SUMS. The workspace script
scripts/dev/ovr-sdk-ci.py makes and uploads the bundle.

    OVR_SDK_DEPLOY_KEY=<private key> fetch-ovr-sdk.py --repo OWNER/REPO \\
        --sha256 HEX --size N --dest DIR

Link DIR/VrApi to third_party/ovr_sdk_mobile/VrApi afterwards. Stdlib only.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import posixpath
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

# GitHub's published ed25519 host key (api.github.com/meta), pinned so a
# spoofed host never receives the key's signature or serves a bundle.
GITHUB_HOST_KEY = "github.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl"
LFS_MEDIA = "application/vnd.git-lfs+json"
# android/app/build.gradle's goVrFiles.
REQUIRED = ["VrApi/Include/VrApi.h", "VrApi/Include/VrApi_Helpers.h",
            "VrApi/Include/VrApi_Input.h", "VrApi/Libs/Android/arm64-v8a/Release/libvrapi.so"]


class Fail(Exception):
    pass


def mask(value: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") == "true" and value:
        print(f"::add-mask::{value}", flush=True)


def lfs_token(repo: str, key: str) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        key_file, known = Path(tmp, "key"), Path(tmp, "known_hosts")
        key_file.write_text(key.strip() + "\n")
        key_file.chmod(0o600)
        known.write_text(GITHUB_HOST_KEY + "\n")
        cmd = ["ssh", "-F", "/dev/null", "-i", str(key_file), "-o", "IdentitiesOnly=yes",
               "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
               "-o", f"UserKnownHostsFile={known}", "-o", "HostKeyAlgorithms=ssh-ed25519",
               "git@github.com", "git-lfs-authenticate", f"{repo}.git", "download"]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if res.returncode != 0:
        raise Fail(f"git-lfs-authenticate on {repo} failed ({res.stderr.strip()}); "
                   "is OVR_SDK_DEPLOY_KEY a deploy key of that repo?")
    auth = json.loads(res.stdout)
    for value in auth.get("header", {}).values():
        mask(value)
    return auth


def request(url: str, headers: dict, body: bytes | None, limit: int) -> bytes:
    req = urllib.request.Request(url, data=body, headers=headers,
                                 method="POST" if body is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = resp.read(limit + 1)
    except urllib.error.HTTPError as e:
        raise Fail(f"{url.split('?')[0]}: HTTP {e.code}") from None
    if len(data) > limit:
        raise Fail(f"{url.split('?')[0]}: response larger than {limit} bytes")
    return data


def download(repo: str, key: str, oid: str, size: int) -> bytes:
    auth = lfs_token(repo, key)
    headers = dict(auth.get("header", {}))
    headers.update({"Accept": LFS_MEDIA, "Content-Type": LFS_MEDIA})
    batch = json.dumps({"operation": "download", "transfers": ["basic"], "hash_algo": "sha256",
                        "objects": [{"oid": oid, "size": size}]}).encode()
    obj = json.loads(request(auth["href"].rstrip("/") + "/objects/batch", headers, batch, 1 << 20))["objects"][0]
    if "error" in obj:
        raise Fail(f"LFS object {oid} on {repo}: {obj['error']}; run "
                   "scripts/dev/ovr-sdk-ci.py bundle and publish in the workspace repo")
    action = obj["actions"]["download"]
    for value in action.get("header", {}).values():
        mask(value)
    data = request(action["href"], dict(action.get("header", {})), None, size)
    if len(data) != size or hashlib.sha256(data).hexdigest() != oid:
        raise Fail(f"bundle is {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}; "
                   f"the workflow pins {size} bytes, sha256 {oid}")
    return data


def unpack(data: bytes, dest: Path) -> list[str]:
    if dest.exists() and any(dest.iterdir()):
        raise Fail(f"{dest} is not empty")
    names = []
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for m in tar.getmembers():
            norm = posixpath.normpath(m.name)
            if not m.isfile() or norm != m.name or norm.startswith(("/", "../")) or norm == "..":
                raise Fail(f"bundle member {m.name!r} is not a plain relative file")
            out = dest / norm
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(tar.extractfile(m).read())
            names.append(norm)
    return names


def check_sums(dest: Path, names: list[str]) -> None:
    listed = {}
    for line in (dest / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        listed[name.lstrip("*")] = digest.lower()
    for name, digest in listed.items():
        f = dest / name
        if not f.is_file() or hashlib.sha256(f.read_bytes()).hexdigest() != digest:
            raise Fail(f"SHA256SUMS: {name} is missing or does not match")
    unlisted = sorted(set(names) - set(listed) - {"SHA256SUMS"})
    if unlisted:
        raise Fail(f"SHA256SUMS does not list {', '.join(unlisted)}")
    missing = [f for f in REQUIRED if f not in listed]
    if missing:
        raise Fail(f"bundle lacks {', '.join(missing)}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--size", required=True, type=int)
    ap.add_argument("--dest", required=True, type=Path)
    args = ap.parse_args(argv)
    key = os.environ.get("OVR_SDK_DEPLOY_KEY", "")
    try:
        if not key.strip():
            raise Fail("OVR_SDK_DEPLOY_KEY is empty: set the secret to the private half of the "
                       f"read-only deploy key on {args.repo}")
        data = download(args.repo, key, args.sha256.lower(), args.size)
        args.dest.mkdir(parents=True, exist_ok=True)
        names = unpack(data, args.dest)
        check_sums(args.dest, names)
    except Fail as e:
        print(f"::error::fetch-ovr-sdk: {e}", file=sys.stderr)
        return 1
    first = (args.dest / "PROVENANCE.txt").read_text().splitlines()[0] if (args.dest / "PROVENANCE.txt").is_file() else ""
    print(f"fetched {args.sha256} ({args.size} bytes) from {args.repo}: {len(names)} files, "
          f"SHA256SUMS OK\n{first}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
