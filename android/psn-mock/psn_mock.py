#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""A mock of the PSN sign-in service the Android app talks to (PLE-284).

It mirrors the paths of Sony's private Remote Play flow closely enough that the
app only swaps a host name: an authorize page (password and passkey), the
redirect that carries a code, the token exchange, the token-info lookup and the
console list. Everything it issues is fake by construction and is never logged.

A scenario is chosen per sign-in by the account name, not by global state, so
the emulator and a phone can test different failures at the same time. The part
before `+` or `@` names the scenario: `token-refused+run7@mock` behaves as
`token-refused`, and any other name behaves as `ok`. See README.md.

Like Sony, a completed sign-in leaves a session cookie in the browser, so opening
the authorize page again redirects with a fresh code and no form (PLE-323: the
app reopens the sign-in when the user leaves the tab without its code).
POST /__mock/sessions/clear forgets every session, so a driver run starts signed out.

PLE-321: the mock also plays the PSN side of a remote connection, so the app's
candidate exchange runs without a console: the push server lookup, the push
WebSocket, Remote Play session creation, the console wake command and the
session messages. A mock console answers on the push socket in the order the
app awaits: it joins, posts customData1 and OFFERs; it answers the app's OFFER
with RESULT and the app's ACCEPT with its own ACCEPT, and once the app RESULTs
that ACCEPT it OFFERs again for the data channel. Its candidates are
documentation addresses (192.0.2.0/24, 198.51.100.0/24), so punching never
succeeds; the exchange up to the punch is what this covers.
"""

from __future__ import annotations

import argparse
import base64
import struct
import hashlib
import html
import json
import os
import secrets
import sys
import threading
import time
from dataclasses import dataclass, field
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa

CLIENT_ID = "ba495a24-818c-472b-b12d-ff231c1b5745"
REDIRECT_PATH = "/remoteplay/redirect"
SESSION_COOKIE = "mock_session"
DEBUG_CERT_SHA256 = "52717a10c7dd74c22c7d1ce10fd0254da2f65a23621bb82aa72157b1c533a01c"
# PLE-568: the app became fi.madekivi.pleikkari; the old id stays listed so a build from a branch
# that predates the rename still gets a verified app link.
DEFAULT_PACKAGES = ("fi.madekivi.pleikkari.psnmock", "com.metallic.chiaki.psnmock")

SESSIONS_PREFIX = "/api/sessionManager/v1/remotePlaySessions/"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B85"
SESSION_CREATED = "psn:sessionManager:sys:remotePlaySession:created"
MEMBER_CREATED = "psn:sessionManager:sys:rps:members:created"
CUSTOM_DATA = "psn:sessionManager:sys:rps:customData1:updated"
SESSION_MESSAGE = "psn:sessionManager:sys:rps:sessionMessage:created"

SCENARIOS = {
    "ok": "Signs in and lists one PS5",
    "expired-code": "The code has expired before the app exchanges it",
    "code-used": "The code was already exchanged once",
    "cancel": "Signing in as this account cancels, like the Cancel button",
    "network-drop": "The token exchange stalls past the app's timeout, then the connection closes",
    "token-refused": "Sign-in works; every later token refresh is refused",
    "empty-consoles": "Sign-in works; the console list is empty",
    "consoles-500": "Sign-in works; the console list fails with a server error",
}


def scenario_of(account: str) -> str:
    base = account.strip().lower().split("@", 1)[0].split("+", 1)[0]
    return base if base in SCENARIOS else "ok"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def b64url_decode(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


# --- tokens -------------------------------------------------------------------
# Tokens carry their account so they survive a restart of the mock; they are not
# secrets and the mock accepts any well-formed one. Their prefix says so.

def make_token(kind: str, account: str) -> str:
    return f"mock-{kind}.{b64url(account.encode())}.{secrets.token_hex(8)}"


def token_account(kind: str, token: str) -> str | None:
    parts = token.split(".")
    if len(parts) != 3 or parts[0] != f"mock-{kind}":
        return None
    try:
        return b64url_decode(parts[1]).decode()
    except ValueError:
        return None


def user_id_of(account: str) -> str:
    return str(int.from_bytes(hashlib.sha256(account.encode()).digest()[:8], "big") >> 1)


# --- CBOR and WebAuthn --------------------------------------------------------

def cbor_decode(data: bytes, offset: int = 0):
    """Decode one CBOR item; returns (value, next_offset). Enough for WebAuthn."""
    initial = data[offset]
    major, info = initial >> 5, initial & 0x1F
    offset += 1
    if info < 24:
        arg = info
    elif info in (24, 25, 26, 27):
        size = 1 << (info - 24)
        arg = int.from_bytes(data[offset:offset + size], "big")
        offset += size
    else:
        raise ValueError("indefinite CBOR lengths are not supported")
    if major == 0:
        return arg, offset
    if major == 1:
        return -1 - arg, offset
    if major in (2, 3):
        chunk = data[offset:offset + arg]
        return (chunk if major == 2 else chunk.decode()), offset + arg
    if major == 4:
        items = []
        for _ in range(arg):
            item, offset = cbor_decode(data, offset)
            items.append(item)
        return items, offset
    if major == 5:
        result = {}
        for _ in range(arg):
            key, offset = cbor_decode(data, offset)
            result[key], offset = cbor_decode(data, offset)
        return result, offset
    if major == 7:
        return {20: False, 21: True, 22: None}.get(arg), offset
    raise ValueError(f"unsupported CBOR major type {major}")


def cose_to_public_key(cose: dict):
    if cose.get(1) == 2 and cose.get(3) == -7 and cose.get(-1) == 1:
        x, y = cose[-2], cose[-3]
        return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + x + y)
    if cose.get(1) == 3 and cose.get(3) == -257:
        return rsa.RSAPublicNumbers(int.from_bytes(cose[-2], "big"), int.from_bytes(cose[-1], "big")).public_key()
    raise ValueError("only ES256 and RS256 passkeys are supported")


class WebAuthnError(ValueError):
    pass


def check_client_data(client_data_json: bytes, kind: str, challenge: str, origin: str) -> None:
    client = json.loads(client_data_json)
    if client.get("type") != kind:
        raise WebAuthnError(f"clientData type is {client.get('type')!r}")
    if client.get("challenge") != challenge:
        raise WebAuthnError("challenge mismatch")
    if client.get("origin") != origin:
        raise WebAuthnError(f"origin {client.get('origin')!r} is not {origin!r}")


def check_auth_data(auth_data: bytes, rp_id: str) -> int:
    if len(auth_data) < 37 or auth_data[:32] != hashlib.sha256(rp_id.encode()).digest():
        raise WebAuthnError("rpIdHash does not match")
    flags = auth_data[32]
    if not flags & 0x01:
        raise WebAuthnError("user presence flag is not set")
    return flags


def verify_registration(attestation_object: bytes, client_data_json: bytes, challenge: str, rp_id: str, origin: str) -> tuple[bytes, bytes]:
    """Returns (credential id, SPKI DER public key). Attestation statements are not checked."""
    check_client_data(client_data_json, "webauthn.create", challenge, origin)
    attestation, _ = cbor_decode(attestation_object)
    auth_data = attestation["authData"]
    if not check_auth_data(auth_data, rp_id) & 0x40:
        raise WebAuthnError("no attested credential data")
    cred_len = int.from_bytes(auth_data[53:55], "big")
    credential_id = auth_data[55:55 + cred_len]
    cose, _ = cbor_decode(auth_data, 55 + cred_len)
    public_key = cose_to_public_key(cose)
    der = public_key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return credential_id, der


def verify_assertion(public_key_der: bytes, auth_data: bytes, client_data_json: bytes, signature: bytes, challenge: str, rp_id: str, origin: str) -> None:
    check_client_data(client_data_json, "webauthn.get", challenge, origin)
    check_auth_data(auth_data, rp_id)
    public_key = serialization.load_der_public_key(public_key_der)
    signed = auth_data + hashlib.sha256(client_data_json).digest()
    try:
        if isinstance(public_key, ec.EllipticCurvePublicKey):
            public_key.verify(signature, signed, ec.ECDSA(hashes.SHA256()))
        else:
            public_key.verify(signature, signed, padding.PKCS1v15(), hashes.SHA256())
    except InvalidSignature as exc:
        raise WebAuthnError("signature does not verify") from exc


# --- state --------------------------------------------------------------------

@dataclass
class Authorization:
    redirect_uri: str
    scope: str
    created: float


@dataclass
class Code:
    account: str
    redirect_uri: str
    expires: float
    used: bool = False


class PushSocket:
    """The server side of a push WebSocket: unmasked frames out, locked so threads do not interleave."""

    def __init__(self, wfile):
        self.wfile = wfile
        self.lock = threading.Lock()

    def send_frame(self, opcode: int, payload: bytes) -> None:
        size = len(payload)
        if size < 126:
            head = struct.pack("!BB", 0x80 | opcode, size)
        elif size < 1 << 16:
            head = struct.pack("!BBH", 0x80 | opcode, 126, size)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, size)
        with self.lock:
            self.wfile.write(head + payload)
            self.wfile.flush()

    def send_json(self, value) -> bool:
        try:
            self.send_frame(0x1, json.dumps(value).encode())
            return True
        except OSError:
            return False


def read_ws_frame(rfile) -> tuple[int, bytes] | None:
    """One client frame (clients always mask), or None at EOF."""
    head = rfile.read(2)
    if len(head) < 2:
        return None
    opcode, size = head[0] & 0x0F, head[1] & 0x7F
    if size == 126:
        size = struct.unpack("!H", rfile.read(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", rfile.read(8))[0]
    mask = rfile.read(4) if head[1] & 0x80 else bytes(4)
    data = rfile.read(size)
    return opcode, bytes(b ^ mask[i % 4] for i, b in enumerate(data))


def signal_payload(message: dict) -> str:
    return "ver=1.0, type=text, body=" + json.dumps(message, separators=(",", ":"))


@dataclass
class RemoteSession:
    account: str
    account_id: str
    console_duid: str = ""
    channel: int = 0  # 0 control, 1 data, 2 both exchanged
    console_req: int = 0
    console_accept_req: int = 0
    client_sid: int = 0


@dataclass
class State:
    hosts: dict[str, bool]  # host -> serves assetlinks.json
    packages: tuple[str, ...]
    credentials_file: Path | None
    code_lifetime: float = 300.0
    drop_seconds: float = 20.0
    lock: threading.Lock = field(default_factory=threading.Lock)
    authorizations: dict[str, Authorization] = field(default_factory=dict)
    codes: dict[str, Code] = field(default_factory=dict)
    challenges: dict[str, tuple[str, str, float]] = field(default_factory=dict)  # challenge -> (kind, subject, time)
    credentials: dict[str, dict] = field(default_factory=dict)  # "rp_id/credential id" -> {account, key}
    sessions: dict[str, str] = field(default_factory=dict)  # session cookie -> account
    events: list[dict] = field(default_factory=list)
    echo: bool = True
    pushes: dict[str, list[PushSocket]] = field(default_factory=dict)  # account -> open push sockets
    remote_sessions: dict[str, RemoteSession] = field(default_factory=dict)  # sessionId -> session

    def push(self, account: str, data_type: str, data: dict) -> int:
        """Sends a notification to every push socket of the account; returns how many took it."""
        with self.lock:
            sockets = list(self.pushes.get(account, []))
        notification = {"dataType": data_type, "body": {"data": data}}
        return sum(socket.send_json(notification) for socket in sockets)

    def console_says(self, session_id: str, session: RemoteSession, message: dict) -> None:
        delivered = self.push(session.account, SESSION_MESSAGE, {
            "sessionId": session_id,
            "sessionMessage": {"payload": signal_payload(message),
                               "from": {"deviceUniqueId": session.console_duid, "platform": "PS5"}},
        })
        self.event("console_signal", action=message["action"], reqId=message["reqId"], channel=session.channel, delivered=delivered)

    def console_offer(self, session_id: str, session: RemoteSession) -> None:
        session.console_req += 1
        self.console_says(session_id, session, {"action": "OFFER", "reqId": session.console_req, "error": 0,
                                                "connRequest": console_conn_request(session)})

    def __post_init__(self) -> None:
        if self.credentials_file and self.credentials_file.exists():
            self.credentials = json.loads(self.credentials_file.read_text())

    def event(self, kind: str, **fields) -> None:
        entry = {"seq": 0, "time": round(time.time(), 3), "event": kind, **fields}
        with self.lock:
            entry["seq"] = len(self.events) + 1
            self.events.append(entry)
            del self.events[:-2000]
        if self.echo:
            print(json.dumps(entry), flush=True)

    def redirect_uris(self) -> set[str]:
        return {f"https://{host}{REDIRECT_PATH}" for host in self.hosts}

    def issue_code(self, account: str, redirect_uri: str) -> str:
        scenario = scenario_of(account)
        code = secrets.token_urlsafe(6)
        now = time.time()
        with self.lock:
            self.codes[code] = Code(
                account=account,
                redirect_uri=redirect_uri,
                expires=now - 1 if scenario == "expired-code" else now + self.code_lifetime,
                used=scenario == "code-used",
            )
        return code

    def save_credentials(self) -> None:
        if self.credentials_file:
            self.credentials_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.credentials_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.credentials, indent=1))
            tmp.replace(self.credentials_file)

    def new_challenge(self, kind: str, subject: str) -> str:
        challenge = b64url(secrets.token_bytes(32))
        with self.lock:
            now = time.time()
            for key in [k for k, (_, _, t) in self.challenges.items() if now - t > 600]:
                del self.challenges[key]
            self.challenges[challenge] = (kind, subject, now)
        return challenge

    def take_challenge(self, challenge: str, kind: str) -> str:
        with self.lock:
            entry = self.challenges.pop(challenge, None)
        if not entry or entry[0] != kind:
            raise WebAuthnError("unknown or reused challenge")
        return entry[1]


# --- pages --------------------------------------------------------------------

PAGE_STYLE = """
body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#111}
main{max-width:420px;margin:0 auto;padding:24px}
.mock{background:#c62828;color:#fff;font-weight:700;padding:8px 12px;text-align:center}
h1{font-size:22px}label{display:block;margin:12px 0 4px}
input{width:100%;box-sizing:border-box;font-size:17px;padding:10px}
button{width:100%;font-size:17px;padding:12px;margin-top:14px;border:0;border-radius:6px}
.primary{background:#0070d1;color:#fff}.secondary{background:#e3e6ea}
.error{color:#b00020}details{margin-top:24px;font-size:14px}code{font-size:13px}
"""

AUTHORIZE_SCRIPT = """
const b64u = b => btoa(String.fromCharCode(...new Uint8Array(b))).replace(/\\+/g,'-').replace(/\\//g,'_').replace(/=+$/,'');
const unb64u = s => Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')), c => c.charCodeAt(0));
const status = t => { document.getElementById('status').textContent = t; };
async function post(path, body) {
  const r = await fetch(path, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || r.status);
  return j;
}
async function createPasskey() {
  try {
    const account = document.getElementById('account').value.trim();
    if (!account) { status('Enter a sign-in ID to create a passkey for.'); return; }
    const o = await post('/webauthn/register/options', {account});
    const cred = await navigator.credentials.create({publicKey: {
      challenge: unb64u(o.challenge), rp: {id: o.rpId, name: 'PSN mock'},
      user: {id: unb64u(o.userId), name: account, displayName: account},
      pubKeyCredParams: [{type:'public-key', alg:-7}, {type:'public-key', alg:-257}],
      authenticatorSelection: {residentKey:'required', userVerification:'preferred'}, attestation:'none'}});
    await post('/webauthn/register', {challenge:o.challenge, id:cred.id,
      clientDataJSON:b64u(cred.response.clientDataJSON), attestationObject:b64u(cred.response.attestationObject)});
    status('Passkey created. Sign in with it now.');
  } catch (e) { status('Passkey not created: ' + e.message); }
}
async function signInWithPasskey() {
  try {
    const o = await post('/webauthn/login/options', {txn: TXN});
    const cred = await navigator.credentials.get({publicKey: {challenge: unb64u(o.challenge), rpId: o.rpId, userVerification:'preferred'}});
    const r = await post('/webauthn/login', {txn:TXN, challenge:o.challenge, id:cred.id,
      clientDataJSON:b64u(cred.response.clientDataJSON), authenticatorData:b64u(cred.response.authenticatorData),
      signature:b64u(cred.response.signature)});
    location.assign(r.redirect);
  } catch (e) { status('Passkey sign-in failed: ' + e.message); }
}
"""


def console_conn_request(session: RemoteSession) -> dict:
    port = 9296 + session.channel
    return {
        "sid": 0x10000 + session.channel, "peerSid": session.client_sid,
        "skey": base64.b64encode(os.urandom(16)).decode(), "natType": 2,
        "candidate": [
            {"type": "LOCAL", "addr": "192.0.2.64", "mappedAddr": "0.0.0.0", "port": port, "mappedPort": 0},
            {"type": "STATIC", "addr": "198.51.100.64", "mappedAddr": "0.0.0.0", "port": port, "mappedPort": 0},
        ],
        "defaultRouteMacAddr": "02:00:00:00:00:64",
        "localPeerAddr": {"accountId": session.account_id, "platform": "PROSPERO"},
        "localHashedId": base64.b64encode(hashlib.sha256(session.console_duid.encode()).digest()[:16]).decode(),
    }


def authorize_page(txn: str, host: str, error: str = "") -> str:
    accounts = "".join(f"<li><code>{html.escape(name)}</code>: {html.escape(text)}</li>" for name, text in SCENARIOS.items())
    error_html = f'<p class="error" id="error">{html.escape(error)}</p>' if error else ""
    return f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sign In (PSN mock)</title><style>{PAGE_STYLE}</style></head><body>
<div class="mock">PSN MOCK · {html.escape(host)} · not Sony</div>
<main><h1>Sign in to PlayStation Network</h1>{error_html}
<form method="post" action="/2.0/oauth/authorize/password">
<input type="hidden" name="txn" value="{html.escape(txn)}">
<label for="account">Sign-in ID</label><input id="account" name="account" autocomplete="username webauthn" value="ok@mock">
<label for="password">Password</label><input id="password" name="password" type="password" autocomplete="current-password">
<button class="primary" id="sign-in" type="submit">Sign In</button>
</form>
<button class="secondary" id="passkey-sign-in" type="button" onclick="signInWithPasskey()">Sign in with a passkey</button>
<button class="secondary" id="passkey-create" type="button" onclick="createPasskey()">Create a passkey</button>
<form method="post" action="/2.0/oauth/authorize/cancel"><input type="hidden" name="txn" value="{html.escape(txn)}">
<button class="secondary" id="cancel" type="submit">Cancel</button></form>
<p id="status" role="status"></p>
<details><summary>Test accounts (the name picks the scenario; any password except <code>wrong</code>)</summary><ul>{accounts}</ul></details>
</main><script>const TXN = {json.dumps(txn)};{AUTHORIZE_SCRIPT}</script></body></html>"""


def error_page(title: str, detail: str) -> str:
    return f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title><style>{PAGE_STYLE}</style></head><body><div class="mock">PSN MOCK</div>
<main><h1>{html.escape(title)}</h1><p>{html.escape(detail)}</p></main></body></html>"""


# --- HTTP ---------------------------------------------------------------------

def make_handler(state: State):
    class Handler(BaseHTTPRequestHandler):
        server_version = "psn-mock"
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # request lines can carry codes; events are logged instead
            pass

        # helpers
        @property
        def host(self) -> str:
            return (self.headers.get("Host") or "").split(":", 1)[0].lower()

        @property
        def origin(self) -> str:
            return f"https://{self.host}"

        def send(self, status: int, body: bytes | str, content_type: str, headers: dict | None = None) -> None:
            data = body.encode() if isinstance(body, str) else body
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            # One handler serves every request on a kept-alive connection: the cookie goes out once.
            if getattr(self, "new_session", None):
                self.send_header("Set-Cookie", f"{SESSION_COOKIE}={self.new_session}; Path=/; Secure; HttpOnly; SameSite=Lax")
                self.new_session = None
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def send_json(self, status: int, value) -> None:
            self.send(status, json.dumps(value), "application/json")

        def send_html(self, status: int, page: str) -> None:
            self.send(status, page, "text/html; charset=utf-8")

        def redirect(self, location: str) -> None:
            self.send(HTTPStatus.FOUND, "", "text/plain", {"Location": location})

        def session_account(self) -> str | None:
            cookies = SimpleCookie(self.headers.get("Cookie", ""))
            token = cookies[SESSION_COOKIE].value if SESSION_COOKIE in cookies else ""
            with state.lock:
                return state.sessions.get(token)

        def body(self) -> bytes:
            return self.rfile.read(int(self.headers.get("Content-Length") or 0))

        def form(self) -> dict[str, str]:
            return {k: v[0] for k, v in parse_qs(self.body().decode(), keep_blank_values=True).items()}

        def known_host(self) -> bool:
            if self.host in state.hosts:
                return True
            self.send_html(HTTPStatus.MISDIRECTED_REQUEST, error_page("Unknown host", "This mock serves only its configured hosts."))
            return False

        def client_authorized(self) -> bool:
            header = self.headers.get("Authorization", "")
            try:
                client_id, _, secret = base64.b64decode(header.removeprefix("Basic ")).decode().partition(":")
            except ValueError:
                client_id, secret = "", ""
            if header.startswith("Basic ") and client_id == CLIENT_ID and secret:
                return True
            state.event("token_rejected", reason="client authentication")
            self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid_client", "error_description": "Bad client credentials"})
            return False

        def bearer_account(self) -> str | None:
            header = self.headers.get("Authorization", "")
            account = token_account("access", header.removeprefix("Bearer ")) if header.startswith("Bearer ") else None
            if account is None:
                self.send_json(HTTPStatus.UNAUTHORIZED, {"error": {"referenceId": "mock", "code": 2241801, "message": "Access token required"}})
            return account

        # routes
        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            url = urlsplit(self.path)
            if url.path == "/healthz":
                return self.send(HTTPStatus.OK, "psn-mock ok\n", "text/plain")
            if not self.known_host():
                return
            if url.path == "/.well-known/assetlinks.json":
                return self.assetlinks()
            if url.path == "/2.0/oauth/authorize":
                return self.authorize(parse_qs(url.query))
            if url.path.startswith("/2.0/oauth/token/"):
                return self.token_info(url.path.rsplit("/", 1)[1])
            if url.path == REDIRECT_PATH:
                # Sony's redirect page is effectively blank; the mock does not help the user either.
                state.event("redirect_page_shown", host=self.host)
                return self.send_html(HTTPStatus.OK, "<!doctype html><html><head><title></title></head><body></body></html>")
            if url.path == "/api/cloudAssistedNavigation/v2/users/me/clients":
                return self.clients()
            if url.path == "/np/serveraddr":
                return self.push_server()
            if url.path == "/np/pushNotification":
                return self.push_socket()
            if url.path == "/__mock/events":
                since = int(parse_qs(url.query).get("since", ["0"])[0])
                with state.lock:
                    events = [e for e in state.events if e["seq"] > since]
                return self.send_json(HTTPStatus.OK, {"events": events})
            if url.path == "/":
                return self.send_html(HTTPStatus.OK, error_page("PSN mock", "Nothing to see here; the app starts at /2.0/oauth/authorize."))
            state.event("not_mocked", method="GET", path=url.path)
            return self.send_json(HTTPStatus.NOT_IMPLEMENTED, {"error": "not mocked", "path": url.path})

        def do_POST(self):
            url = urlsplit(self.path)
            if not self.known_host():
                return
            routes = {
                "/2.0/oauth/authorize/password": self.password_sign_in,
                "/2.0/oauth/authorize/cancel": self.cancel,
                "/2.0/oauth/token": self.token,
                "/__mock/sessions/clear": self.clear_sessions,
                "/webauthn/register/options": self.register_options,
                "/webauthn/register": self.register,
                "/webauthn/login/options": self.login_options,
                "/webauthn/login": self.login,
            }
            route = routes.get(url.path)
            if route:
                return route()
            if url.path == "/api/sessionManager/v1/remotePlaySessions":
                return self.create_remote_session()
            if url.path == "/api/cloudAssistedNavigation/v2/users/me/commands":
                return self.console_command()
            parts = url.path.split("/")
            if len(parts) == 7 and url.path.startswith(SESSIONS_PREFIX) and parts[6] == "sessionMessage":
                return self.session_message(parts[5])
            state.event("not_mocked", method="POST", path=url.path)
            return self.send_json(HTTPStatus.NOT_IMPLEMENTED, {"error": "not mocked", "path": url.path})

        def do_DELETE(self):
            url = urlsplit(self.path)
            if not self.known_host():
                return
            parts = url.path.split("/")
            if len(parts) == 8 and url.path.startswith(SESSIONS_PREFIX) and parts[6:] == ["members", "me"]:
                return self.leave_remote_session(parts[5])
            state.event("not_mocked", method="DELETE", path=url.path)
            return self.send_json(HTTPStatus.NOT_IMPLEMENTED, {"error": "not mocked", "path": url.path})

        def assetlinks(self):
            if not state.hosts[self.host]:
                state.event("assetlinks", host=self.host, served=False)
                return self.send_json(HTTPStatus.NOT_FOUND, {"error": "no assetlinks on this host"})
            state.event("assetlinks", host=self.host, served=True)
            statements = [{
                # handle_all_urls only. Sony's assetlinks do not delegate its passkeys to this app either, and
                # get_login_creds would let a WebView use the mock's passkeys where Sony's never work.
                "relation": ["delegate_permission/common.handle_all_urls"],
                "target": {"namespace": "android_app", "package_name": package,
                           "sha256_cert_fingerprints": [":".join(DEBUG_CERT_SHA256[i:i + 2] for i in range(0, 64, 2)).upper()]},
            } for package in state.packages]
            return self.send_json(HTTPStatus.OK, statements)

        def authorize(self, query: dict[str, list[str]]):
            one = lambda name: query.get(name, [""])[0]
            problems = []
            if one("response_type") != "code":
                problems.append("response_type must be code")
            if one("client_id") != CLIENT_ID:
                problems.append("unknown client_id")
            if one("redirect_uri") not in state.redirect_uris():
                problems.append("redirect_uri is not registered for this client")
            if problems:
                state.event("authorize_rejected", host=self.host, problems=problems)
                return self.send_html(HTTPStatus.BAD_REQUEST, error_page("Sign-in request rejected", "; ".join(problems)))
            txn = secrets.token_urlsafe(12)
            with state.lock:
                state.authorizations[txn] = Authorization(one("redirect_uri"), one("scope"), time.time())
            account = self.session_account()
            if account is not None:
                state.event("authorize_session", host=self.host, account=account)
                return self.redirect(self.complete_sign_in(state.authorizations[txn], account, "session"))
            state.event("authorize_page", host=self.host, redirect_host=urlsplit(one("redirect_uri")).hostname)
            return self.send_html(HTTPStatus.OK, authorize_page(txn, self.host))

        def authorization(self, txn: str) -> Authorization | None:
            with state.lock:
                return state.authorizations.get(txn)

        def complete_sign_in(self, auth: Authorization, account: str, method: str) -> str:
            if scenario_of(account) == "cancel":
                state.event("sign_in_cancelled", account=account, method=method)
                return auth.redirect_uri + "?" + urlencode({"error": "access_denied", "error_description": "User cancelled"})
            code = state.issue_code(account, auth.redirect_uri)
            self.new_session = make_token("session", account)
            with state.lock:
                state.sessions[self.new_session] = account
            state.event("code_issued", account=account, scenario=scenario_of(account), method=method,
                        redirect_host=urlsplit(auth.redirect_uri).hostname)
            return auth.redirect_uri + "?" + urlencode({"code": code, "cid": secrets.token_hex(8)})

        def password_sign_in(self):
            form = self.form()
            auth = self.authorization(form.get("txn", ""))
            if auth is None:
                return self.send_html(HTTPStatus.BAD_REQUEST, error_page("Session expired", "Start signing in again from the app."))
            account = form.get("account", "").strip()
            if not account or not form.get("password") or form.get("password") == "wrong":
                state.event("password_rejected", account=account)
                return self.send_html(HTTPStatus.OK, authorize_page(form["txn"], self.host, "The sign-in ID or password is incorrect."))
            return self.redirect(self.complete_sign_in(auth, account, "password"))

        def clear_sessions(self):
            with state.lock:
                count = len(state.sessions)
                state.sessions.clear()
            state.event("sessions_cleared", count=count)
            return self.send_json(HTTPStatus.OK, {"cleared": count})

        def cancel(self):
            auth = self.authorization(self.form().get("txn", ""))
            if auth is None:
                return self.send_html(HTTPStatus.BAD_REQUEST, error_page("Session expired", "Start signing in again from the app."))
            state.event("sign_in_cancelled", method="cancel-button")
            return self.redirect(auth.redirect_uri + "?" + urlencode({"error": "access_denied", "error_description": "User cancelled"}))

        def token(self):
            form = self.form()
            if not self.client_authorized():
                return
            grant = form.get("grant_type")
            if grant == "authorization_code":
                return self.exchange_code(form)
            if grant == "refresh_token":
                return self.refresh(form)
            state.event("token_rejected", reason=f"grant_type {grant!r}")
            return self.send_json(HTTPStatus.BAD_REQUEST, {"error": "unsupported_grant_type"})

        def exchange_code(self, form: dict[str, str]):
            with state.lock:
                code = state.codes.get(form.get("code", ""))
                reason = None
                if code is None:
                    reason = "unknown code"
                elif code.used:
                    reason = "code already used"
                elif code.expires < time.time():
                    reason = "code expired"
                elif form.get("redirect_uri") != code.redirect_uri:
                    reason = "redirect_uri differs from the authorize request"
                elif scenario_of(code.account) != "network-drop":
                    code.used = True
            if reason:
                state.event("exchange_failed", reason=reason, account=code.account if code else None)
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid_grant", "error_description": "Invalid authorization code", "error_code": 4165})
            if scenario_of(code.account) == "network-drop":
                state.event("exchange_dropped", account=code.account, after_seconds=state.drop_seconds)
                time.sleep(state.drop_seconds)
                self.close_connection = True
                return
            state.event("exchange_ok", account=code.account)
            return self.send_json(HTTPStatus.OK, self.token_response(code.account))

        def refresh(self, form: dict[str, str]):
            account = token_account("refresh", form.get("refresh_token", ""))
            if account is None or scenario_of(account) == "token-refused":
                state.event("refresh_refused", account=account)
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid_grant", "error_description": "Invalid refresh token", "error_code": 4159})
            state.event("refresh_ok", account=account)
            return self.send_json(HTTPStatus.OK, self.token_response(account))

        def token_response(self, account: str) -> dict:
            return {
                "access_token": make_token("access", account),
                "token_type": "bearer",
                "expires_in": 3599,
                "scope": "psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update",
                "refresh_token": make_token("refresh", account),
                "refresh_token_expires_in": 5183999,
            }

        def token_info(self, token: str):
            account = token_account("access", token)
            if account is None:
                return self.send_json(HTTPStatus.NOT_FOUND, {"error": "invalid_token"})
            state.event("token_info", account=account)
            return self.send_json(HTTPStatus.OK, {
                "scopes": "psn:clientapp", "expiration": "2099-01-01T00:00:00.000Z", "client_id": CLIENT_ID,
                "grant_type": "authorization_code", "user_id": user_id_of(account),
                "online_id": "mock_" + hashlib.sha256(account.encode()).hexdigest()[:8],
                "country_code": "FI", "language_code": "en", "valid_for_duration_seconds": 3599,
            })

        def clients(self):
            account = self.bearer_account()
            if account is None:
                return
            scenario = scenario_of(account)
            state.event("console_list", account=account, scenario=scenario)
            if scenario == "consoles-500":
                return self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": {"referenceId": "mock", "code": 2289153, "message": "Internal Server Error"}})
            clients = [] if scenario == "empty-consoles" else [
                {"duid": "0000000700410080" + hashlib.sha256(account.encode()).hexdigest()[:32], "platform": "PS5",
                 "device": {"name": "PS5 mock", "enabledFeatures": ["remotePlay"]}},
                {"duid": "000000070041008" + "0" * 33, "platform": "PS5",
                 "device": {"name": "Remote play disabled", "enabledFeatures": []}},
            ]
            return self.send_json(HTTPStatus.OK, {"clients": clients, "totalResults": len(clients)})

        # remote connection (PLE-321)
        def push_server(self):
            if self.bearer_account() is None:
                return
            return self.send_json(HTTPStatus.OK, {"fqdn": self.host, "keepAliveStatus": {"type": 3}})

        def push_socket(self):
            account = self.bearer_account()
            if account is None:
                return
            key = self.headers.get("Sec-WebSocket-Key", "")
            if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": "websocket upgrade required"})
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            head = ["HTTP/1.1 101 Switching Protocols", "Upgrade: websocket", "Connection: Upgrade", f"Sec-WebSocket-Accept: {accept}"]
            if self.headers.get("Sec-WebSocket-Protocol"):
                head.append("Sec-WebSocket-Protocol: np-pushpacket")
            self.wfile.write(("\r\n".join(head) + "\r\n\r\n").encode())
            self.wfile.flush()
            self.close_connection = True
            socket = PushSocket(self.wfile)
            with state.lock:
                state.pushes.setdefault(account, []).append(socket)
            state.event("push_open", account=account)
            try:
                while (frame := read_ws_frame(self.rfile)) is not None:
                    opcode, payload = frame
                    if opcode == 0x8:
                        socket.send_frame(0x8, payload[:2])
                        break
                    if opcode == 0x9:
                        socket.send_frame(0xA, payload)
            except OSError:
                pass
            finally:
                with state.lock:
                    state.pushes[account].remove(socket)
                state.event("push_closed", account=account)

        def create_remote_session(self):
            account = self.bearer_account()
            if account is None:
                return
            try:
                member = self.json_body()["remotePlaySessions"][0]["members"][0]
                push_contexts = [p["pushContextId"] for p in member["pushContexts"]]
            except (KeyError, IndexError, TypeError):
                state.event("remote_session_rejected", account=account)
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": 2289154, "message": "bad session request"}})
            session_id = "mock-" + secrets.token_hex(8)
            session = RemoteSession(account=account, account_id=user_id_of(account))
            client = {"accountId": session.account_id, "deviceUniqueId": "mock-client", "platform": "REMOTE_PLAY"}
            with state.lock:
                state.remote_sessions[session_id] = session
            state.event("remote_session_created", account=account, session=session_id, push_contexts=len(push_contexts))
            self.send_json(HTTPStatus.OK, {"remotePlaySessions": [{"sessionId": session_id, "members": [client]}]})
            state.push(account, SESSION_CREATED, {"sessionId": session_id})
            state.push(account, MEMBER_CREATED, {"sessionId": session_id, "members": [client]})

        def console_command(self):
            account = self.bearer_account()
            if account is None:
                return
            detail = self.json_body().get("commandDetail", {})
            try:
                params = json.loads(detail["parameters"]["initialParams"])
                session_id = str(params["sessionId"])
            except (KeyError, TypeError, ValueError):
                params, session_id = {}, ""
            with state.lock:
                session = state.remote_sessions.get(session_id)
            if session is None or session.account != account:
                state.event("console_command_rejected", account=account)
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": 2289155, "message": "unknown session"}})
            session.console_duid = str(detail.get("duid", ""))
            state.event("console_woken", account=account, session=session_id, duid=session.console_duid,
                        has_data=bool(params.get("data1")) and bool(params.get("data2")))
            self.send(HTTPStatus.NO_CONTENT, b"", "application/json")
            state.push(account, MEMBER_CREATED, {"sessionId": session_id, "members": [
                {"accountId": session.account_id, "deviceUniqueId": session.console_duid, "platform": "PS5"}]})
            # customData1 is the base64 of the base64 of 16 bytes, as the console posts it.
            inner = base64.b64encode(os.urandom(16))
            state.push(account, CUSTOM_DATA, {"sessionId": session_id, "customData1": base64.b64encode(inner).decode()})
            state.console_offer(session_id, session)

        def session_message(self, session_id: str):
            account = self.bearer_account()
            if account is None:
                return
            with state.lock:
                session = state.remote_sessions.get(session_id)
            if session is None or session.account != account:
                return self.send_json(HTTPStatus.NOT_FOUND, {"error": {"code": 2289156, "message": "unknown session"}})
            envelope = self.json_body()
            try:
                body = str(envelope["payload"]).split("body=", 1)[1]
                message = json.loads(body.replace('"localPeerAddr":,', '"localPeerAddr":null,'))
                action, req_id = str(message["action"]), int(message["reqId"])
            except (IndexError, KeyError, TypeError, ValueError):
                state.event("client_signal_invalid", session=session_id)
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": 2289157, "message": "bad payload"}})
            conn_request = message.get("connRequest") or {}
            to = (envelope.get("to") or [{}])[0]
            state.event("client_signal", session=session_id, action=action, reqId=req_id, channel=session.channel,
                        candidates=len(conn_request.get("candidate", [])),
                        to_console=to.get("deviceUniqueId") == session.console_duid.lower())
            self.send(HTTPStatus.NO_CONTENT, b"", "application/json")
            if action == "OFFER":
                session.client_sid = int(conn_request.get("sid", 0))
                state.console_says(session_id, session, {"action": "RESULT", "reqId": req_id, "error": 0, "connRequest": {}})
            elif action == "ACCEPT":
                session.console_req += 1
                session.console_accept_req = session.console_req
                state.console_says(session_id, session, {"action": "ACCEPT", "reqId": session.console_req, "error": 0,
                                                         "connRequest": console_conn_request(session)})
            elif action == "RESULT" and session.console_accept_req and req_id == session.console_accept_req:
                session.console_accept_req = 0
                state.event("channel_exchanged", session=session_id, channel=session.channel)
                session.channel += 1
                if session.channel == 1:
                    state.console_offer(session_id, session)

        def leave_remote_session(self, session_id: str):
            account = self.bearer_account()
            if account is None:
                return
            with state.lock:
                session = state.remote_sessions.pop(session_id, None)
            state.event("remote_session_left", account=account, session=session_id, known=session is not None)
            return self.send(HTTPStatus.NO_CONTENT, b"", "application/json")

        # passkeys
        def json_body(self) -> dict:
            try:
                return json.loads(self.body() or b"{}")
            except ValueError:
                return {}

        def webauthn(self, action):
            try:
                return self.send_json(HTTPStatus.OK, action())
            except (WebAuthnError, KeyError, ValueError) as exc:
                state.event("passkey_failed", host=self.host, reason=str(exc))
                return self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})

        def register_options(self):
            body = self.json_body()

            def action():
                account = str(body["account"]).strip()
                if not account:
                    raise WebAuthnError("account is required")
                return {"challenge": state.new_challenge("create", account), "rpId": self.host, "userId": b64url(account.encode())}
            return self.webauthn(action)

        def register(self):
            body = self.json_body()

            def action():
                account = state.take_challenge(body["challenge"], "create")
                cred_id, key = verify_registration(b64url_decode(body["attestationObject"]), b64url_decode(body["clientDataJSON"]),
                                                   body["challenge"], self.host, self.origin)
                with state.lock:
                    state.credentials[f"{self.host}/{b64url(cred_id)}"] = {"account": account, "key": b64url(key)}
                    state.save_credentials()
                state.event("passkey_created", host=self.host, account=account)
                return {"ok": True}
            return self.webauthn(action)

        def login_options(self):
            body = self.json_body()

            def action():
                if self.authorization(str(body["txn"])) is None:
                    raise WebAuthnError("sign-in session expired")
                return {"challenge": state.new_challenge("get", str(body["txn"])), "rpId": self.host}
            return self.webauthn(action)

        def login(self):
            body = self.json_body()

            def action():
                txn = state.take_challenge(body["challenge"], "get")
                auth = self.authorization(txn)
                if auth is None or txn != body.get("txn"):
                    raise WebAuthnError("sign-in session expired")
                with state.lock:
                    stored = state.credentials.get(f"{self.host}/{body['id']}")
                if stored is None:
                    raise WebAuthnError("unknown passkey for this host")
                verify_assertion(b64url_decode(stored["key"]), b64url_decode(body["authenticatorData"]),
                                 b64url_decode(body["clientDataJSON"]), b64url_decode(body["signature"]),
                                 body["challenge"], self.host, self.origin)
                return {"redirect": self.complete_sign_in(auth, stored["account"], "passkey")}
            return self.webauthn(action)

    return Handler


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PSN_MOCK_PORT", "18284")))
    parser.add_argument("--verified-host", action="append", default=[], help="host that serves assetlinks.json (repeatable)")
    parser.add_argument("--nolink-host", action="append", default=[], help="host without assetlinks.json (repeatable)")
    parser.add_argument("--package", action="append", default=[], help=f"package named in assetlinks.json (default {DEFAULT_PACKAGES[0]})")
    parser.add_argument("--state-dir", type=Path, default=Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "pleikkari-psn-mock")
    parser.add_argument("--code-lifetime", type=float, default=300.0)
    parser.add_argument("--drop-seconds", type=float, default=20.0, help="how long network-drop stalls; the app times out at 15 s")
    args = parser.parse_args(argv)
    hosts = {h.lower(): True for h in args.verified_host} | {h.lower(): False for h in args.nolink_host}
    if not hosts:
        parser.error("name at least one --verified-host or --nolink-host")
    state = State(hosts=hosts, packages=tuple(args.package) or DEFAULT_PACKAGES,
                  credentials_file=args.state_dir / "passkeys.json",
                  code_lifetime=args.code_lifetime, drop_seconds=args.drop_seconds)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(state))
    server.daemon_threads = True
    state.event("listening", bind=args.bind, port=server.server_address[1], hosts=hosts)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
