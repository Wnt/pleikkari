#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""Unit tests for psn_mock.py: python3 -m unittest discover -s android/psn-mock"""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import re
import socket
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlsplit

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec

import psn_mock

VERIFIED = "verified.test"
NOLINK = "nolink.test"
BASIC = "Basic " + base64.b64encode(f"{psn_mock.CLIENT_ID}:not-the-real-one".encode()).decode()


def cbor_encode(value) -> bytes:
    def head(major: int, arg: int) -> bytes:
        if arg < 24:
            return bytes([major << 5 | arg])
        for info, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if arg < 1 << (8 * size):
                return bytes([major << 5 | info]) + arg.to_bytes(size, "big")
        raise ValueError(arg)

    if isinstance(value, int):
        return head(0, value) if value >= 0 else head(1, -1 - value)
    if isinstance(value, bytes):
        return head(2, len(value)) + value
    if isinstance(value, str):
        return head(3, len(value.encode())) + value.encode()
    if isinstance(value, dict):
        return head(5, len(value)) + b"".join(cbor_encode(k) + cbor_encode(v) for k, v in value.items())
    raise TypeError(value)


class SoftwareAuthenticator:
    def __init__(self, rp_id: str):
        self.rp_id = rp_id
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.cred_id = b"credential-1"

    def auth_data(self, attested: bool) -> bytes:
        flags = 0x01 | 0x04 | (0x40 if attested else 0)
        data = hashlib.sha256(self.rp_id.encode()).digest() + bytes([flags]) + (0).to_bytes(4, "big")
        if attested:
            numbers = self.key.public_key().public_numbers()
            cose = {1: 2, 3: -7, -1: 1, -2: numbers.x.to_bytes(32, "big"), -3: numbers.y.to_bytes(32, "big")}
            data += bytes(16) + len(self.cred_id).to_bytes(2, "big") + self.cred_id + cbor_encode(cose)
        return data

    @staticmethod
    def client_data(kind: str, challenge: str, origin: str) -> bytes:
        return json.dumps({"type": kind, "challenge": challenge, "origin": origin}).encode()

    def create(self, challenge: str, origin: str) -> dict:
        attestation = cbor_encode({"fmt": "none", "attStmt": {}, "authData": self.auth_data(True)})
        return {"challenge": challenge, "id": psn_mock.b64url(self.cred_id),
                "clientDataJSON": psn_mock.b64url(self.client_data("webauthn.create", challenge, origin)),
                "attestationObject": psn_mock.b64url(attestation)}

    def get(self, challenge: str, origin: str) -> dict:
        auth_data = self.auth_data(False)
        client_data = self.client_data("webauthn.get", challenge, origin)
        signature = self.key.sign(auth_data + hashlib.sha256(client_data).digest(), ec.ECDSA(hashes.SHA256()))
        return {"challenge": challenge, "id": psn_mock.b64url(self.cred_id),
                "clientDataJSON": psn_mock.b64url(client_data), "authenticatorData": psn_mock.b64url(auth_data),
                "signature": psn_mock.b64url(signature)}


class MockServerTest(unittest.TestCase):
    def setUp(self):
        self.state = psn_mock.State(hosts={VERIFIED: True, NOLINK: False}, packages=psn_mock.DEFAULT_PACKAGES,
                                    credentials_file=None, drop_seconds=0.2, echo=False)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), psn_mock.make_handler(self.state))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def request(self, method: str, path: str, host: str = VERIFIED, body: bytes | str | None = None, headers: dict | None = None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request(method, path, body=body, headers={"Host": host, **(headers or {})})
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response, data

    def authorize(self, host: str = VERIFIED, redirect_host: str | None = None) -> str:
        query = urlencode({"response_type": "code", "client_id": psn_mock.CLIENT_ID,
                           "redirect_uri": f"https://{redirect_host or host}/remoteplay/redirect", "scope": "psn:clientapp"})
        response, page = self.request("GET", "/2.0/oauth/authorize?" + query, host)
        self.assertEqual(response.status, 200, page)
        return re.search(rb'name="txn" value="([^"]+)"', page).group(1).decode()

    def sign_in(self, account: str, host: str = VERIFIED, password: str = "pw") -> dict[str, str]:
        txn = self.authorize(host)
        response, _ = self.request("POST", "/2.0/oauth/authorize/password", host,
                                   urlencode({"txn": txn, "account": account, "password": password}),
                                   {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(response.status, 302)
        location = urlsplit(response.getheader("Location"))
        self.assertEqual((location.hostname, location.path), (host, "/remoteplay/redirect"))
        return {k: v[0] for k, v in parse_qs(location.query).items()}

    def exchange(self, code: str, host: str = VERIFIED):
        body = urlencode({"grant_type": "authorization_code", "code": code, "scope": "psn:clientapp",
                          "redirect_uri": f"https://{host}/remoteplay/redirect"})
        response, data = self.request("POST", "/2.0/oauth/token", host, body,
                                      {"Authorization": BASIC, "Content-Type": "application/x-www-form-urlencoded"})
        return response.status, json.loads(data) if data else None

    def list_consoles(self, access_token: str):
        response, data = self.request("GET", "/api/cloudAssistedNavigation/v2/users/me/clients?platform=PS5",
                                      headers={"Authorization": "Bearer " + access_token})
        return response.status, json.loads(data)

    def test_assetlinks_only_on_the_verified_host(self):
        response, data = self.request("GET", "/.well-known/assetlinks.json")
        self.assertEqual(response.status, 200)
        statement = json.loads(data)[0]
        self.assertEqual(statement["relation"], ["delegate_permission/common.handle_all_urls"])
        target = statement["target"]
        self.assertEqual(target["package_name"], "fi.madekivi.pleikkari.psnmock")
        self.assertEqual(target["sha256_cert_fingerprints"][0].replace(":", "").lower(), psn_mock.DEBUG_CERT_SHA256)
        response, _ = self.request("GET", "/.well-known/assetlinks.json", NOLINK)
        self.assertEqual(response.status, 404)

    def test_unknown_host_and_unregistered_redirect_are_rejected(self):
        response, _ = self.request("GET", "/2.0/oauth/authorize", "elsewhere.test")
        self.assertEqual(response.status, 421)
        query = urlencode({"response_type": "code", "client_id": psn_mock.CLIENT_ID,
                           "redirect_uri": "https://remoteplay.dl.playstation.net/remoteplay/redirect"})
        response, _ = self.request("GET", "/2.0/oauth/authorize?" + query)
        self.assertEqual(response.status, 400)

    def test_happy_path_is_single_use_and_lists_a_remote_play_console(self):
        code = self.sign_in("ok+t1@mock")["code"]
        status, token = self.exchange(code)
        self.assertEqual(status, 200)
        self.assertNotIn("user_id", token)
        response, info = self.request("GET", "/2.0/oauth/token/" + token["access_token"])
        self.assertEqual(response.status, 200)
        self.assertTrue(json.loads(info)["user_id"].isdigit())
        self.assertEqual(self.exchange(code)[0], 400)
        status, listing = self.list_consoles(token["access_token"])
        self.assertEqual(status, 200)
        self.assertEqual([c["device"]["enabledFeatures"] for c in listing["clients"]], [["remotePlay"], []])

    def test_token_exchange_needs_the_client_credentials(self):
        code = self.sign_in("ok")["code"]
        body = urlencode({"grant_type": "authorization_code", "code": code, "redirect_uri": f"https://{VERIFIED}/remoteplay/redirect"})
        response, _ = self.request("POST", "/2.0/oauth/token", body=body)
        self.assertEqual(response.status, 401)

    def test_redirect_uri_must_match_the_authorize_request(self):
        code = self.sign_in("ok")["code"]
        self.assertEqual(self.exchange(code, NOLINK)[0], 400)

    def test_wrong_password_stays_on_the_page(self):
        txn = self.authorize()
        response, page = self.request("POST", "/2.0/oauth/authorize/password", body=urlencode({"txn": txn, "account": "ok", "password": "wrong"}))
        self.assertEqual(response.status, 200)
        self.assertIn(b"incorrect", page)

    def test_code_scenarios_fail_the_exchange(self):
        for account in ("expired-code@mock", "code-used+x@mock"):
            with self.subTest(account=account):
                status, body = self.exchange(self.sign_in(account)["code"])
                self.assertEqual((status, body["error"]), (400, "invalid_grant"))

    def test_session_cookie_skips_the_form_until_sessions_are_cleared(self):
        txn = self.authorize()
        response, _ = self.request("POST", "/2.0/oauth/authorize/password",
                                   body=urlencode({"txn": txn, "account": "ok+session@mock", "password": "pw"}),
                                   headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(response.status, 302)
        first = parse_qs(urlsplit(response.getheader("Location")).query)["code"][0]
        cookie = response.getheader("Set-Cookie").split(";", 1)[0]
        self.assertTrue(cookie.startswith(psn_mock.SESSION_COOKIE + "="))
        query = urlencode({"response_type": "code", "client_id": psn_mock.CLIENT_ID,
                           "redirect_uri": f"https://{VERIFIED}/remoteplay/redirect", "scope": "psn:clientapp"})
        response, _ = self.request("GET", "/2.0/oauth/authorize?" + query, headers={"Cookie": cookie})
        self.assertEqual(response.status, 302)
        second = parse_qs(urlsplit(response.getheader("Location")).query)["code"][0]
        self.assertNotEqual(first, second)
        self.assertEqual(self.exchange(second)[0], 200)
        response, _ = self.request("POST", "/__mock/sessions/clear")
        self.assertEqual(response.status, 200)
        response, page = self.request("GET", "/2.0/oauth/authorize?" + query, headers={"Cookie": cookie})
        self.assertEqual(response.status, 200)
        self.assertIn(b'name="txn"', page)

    def test_cancel_redirects_with_an_error_and_no_code(self):
        params = self.sign_in("cancel@mock", NOLINK)
        self.assertEqual(params, {"error": "access_denied", "error_description": "User cancelled"})
        txn = self.authorize()
        response, _ = self.request("POST", "/2.0/oauth/authorize/cancel", body=urlencode({"txn": txn}))
        self.assertIn("error=access_denied", response.getheader("Location"))

    def test_network_drop_closes_without_a_response(self):
        code = self.sign_in("network-drop@mock")["code"]
        started = time.monotonic()
        with self.assertRaises((http.client.RemoteDisconnected, ConnectionError)):
            self.exchange(code)
        self.assertGreaterEqual(time.monotonic() - started, 0.2)

    def test_token_refused_fails_refresh_only(self):
        status, token = self.exchange(self.sign_in("token-refused@mock")["code"])
        self.assertEqual(status, 200)
        response, data = self.request("POST", "/2.0/oauth/token", body=urlencode({"grant_type": "refresh_token", "refresh_token": token["refresh_token"]}),
                                      headers={"Authorization": BASIC})
        self.assertEqual(response.status, 400)
        status, ok_token = self.exchange(self.sign_in("ok")["code"])
        response, _ = self.request("POST", "/2.0/oauth/token", body=urlencode({"grant_type": "refresh_token", "refresh_token": ok_token["refresh_token"]}),
                                   headers={"Authorization": BASIC})
        self.assertEqual(response.status, 200)

    def test_listing_scenarios(self):
        _, empty = self.exchange(self.sign_in("empty-consoles@mock")["code"])
        self.assertEqual(self.list_consoles(empty["access_token"]), (200, {"clients": [], "totalResults": 0}))
        _, failing = self.exchange(self.sign_in("consoles-500@mock")["code"])
        self.assertEqual(self.list_consoles(failing["access_token"])[0], 500)
        self.assertEqual(self.list_consoles("not-a-token")[0], 401)

    def test_passkey_register_then_sign_in(self):
        authenticator = SoftwareAuthenticator(VERIFIED)
        origin = f"https://{VERIFIED}"

        def post(path, body, host=VERIFIED):
            response, data = self.request("POST", path, host, json.dumps(body), {"Content-Type": "application/json"})
            return response.status, json.loads(data)

        status, options = post("/webauthn/register/options", {"account": "empty-consoles+pk@mock"})
        self.assertEqual((status, options["rpId"]), (200, VERIFIED))
        self.assertEqual(post("/webauthn/register", authenticator.create(options["challenge"], origin))[0], 200)

        txn = self.authorize()
        _, options = post("/webauthn/login/options", {"txn": txn})
        wrong_origin = authenticator.get(options["challenge"], "https://evil.test")
        self.assertEqual(post("/webauthn/login", {"txn": txn, **wrong_origin})[0], 400)

        _, options = post("/webauthn/login/options", {"txn": txn})
        status, result = post("/webauthn/login", {"txn": txn, **authenticator.get(options["challenge"], origin)})
        self.assertEqual(status, 200, result)
        redirect = urlsplit(result["redirect"])
        self.assertEqual(redirect.hostname, VERIFIED)
        _, token = self.exchange(parse_qs(redirect.query)["code"][0])
        self.assertEqual(self.list_consoles(token["access_token"])[1]["clients"], [])

        # A passkey is bound to its host, as WebAuthn binds it to the relying party.
        _, options = post("/webauthn/login/options", {"txn": self.authorize(NOLINK)}, NOLINK)
        self.assertEqual(post("/webauthn/login", {"txn": txn, **authenticator.get(options["challenge"], f"https://{NOLINK}")}, NOLINK)[0], 400)

    def test_remote_session_candidate_exchange(self):
        """PLE-321: the push socket, session and OFFER/RESULT/ACCEPT order PsnRemoteController awaits."""
        _, token = self.exchange(self.sign_in("ok+remote@mock")["code"])
        bearer = {"Authorization": "Bearer " + token["access_token"], "Content-Type": "application/json"}
        duid = self.list_consoles(token["access_token"])[1]["clients"][0]["duid"]

        def call(method, path, body=None):
            response, data = self.request(method, path, body=json.dumps(body) if body is not None else None, headers=bearer)
            return response.status, json.loads(data) if data else None

        status, address = call("GET", "/np/serveraddr?version=2.1")
        self.assertEqual((status, address["fqdn"]), (200, VERIFIED))

        sock = socket.create_connection(("127.0.0.1", self.server.server_address[1]), timeout=5)
        self.addCleanup(sock.close)
        key = base64.b64encode(b"0123456789abcdef").decode()
        sock.sendall((f"GET /np/pushNotification HTTP/1.1\r\nHost: {VERIFIED}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: np-pushpacket\r\n"
                      f"Authorization: {bearer['Authorization']}\r\n\r\n").encode())
        stream = sock.makefile("rb")
        head = b""
        while not head.endswith(b"\r\n\r\n"):
            head += stream.read(1)
        self.assertIn(b" 101 ", head.split(b"\r\n")[0])
        expected = base64.b64encode(hashlib.sha1((key + psn_mock.WS_GUID).encode()).digest())
        self.assertIn(b"Sec-WebSocket-Accept: " + expected, head)

        def notification():
            opcode, payload = psn_mock.read_ws_frame(stream)
            self.assertEqual(opcode, 1)
            return json.loads(payload)

        def signal():
            value = notification()
            self.assertEqual(value["dataType"], psn_mock.SESSION_MESSAGE)
            return json.loads(value["body"]["data"]["sessionMessage"]["payload"].split("body=", 1)[1])

        status, created = call("POST", "/api/sessionManager/v1/remotePlaySessions", {"remotePlaySessions": [{"members": [
            {"accountId": "me", "deviceUniqueId": "me", "platform": "me", "pushContexts": [{"pushContextId": "ctx"}]}]}]})
        self.assertEqual(status, 200)
        session = created["remotePlaySessions"][0]
        account_id = session["members"][0]["accountId"]
        self.assertEqual([notification()["dataType"] for _ in range(2)], [psn_mock.SESSION_CREATED, psn_mock.MEMBER_CREATED])

        params = json.dumps({"accountId": int(account_id), "roomId": 0, "sessionId": session["sessionId"],
                             "clientType": "Windows", "data1": "AA==", "data2": "AA=="})
        status, _ = call("POST", "/api/cloudAssistedNavigation/v2/users/me/commands", {"commandDetail": {
            "commandType": "remotePlay", "duid": duid, "messageDestination": "SQS",
            "parameters": {"initialParams": params}, "platform": "PS5"}})
        self.assertEqual(status, 204)
        joined = notification()
        self.assertEqual(joined["body"]["data"]["members"][0]["deviceUniqueId"], duid)
        custom = notification()["body"]["data"]["customData1"]
        self.assertEqual(len(base64.b64decode(base64.b64decode(custom))), 16)

        def send(action, req_id, conn_request=None):
            body = json.dumps({"action": action, "reqId": req_id, "error": 0, "connRequest": conn_request or {}})
            status, _ = call("POST", f"/api/sessionManager/v1/remotePlaySessions/{session['sessionId']}/sessionMessage", {
                "channel": "remote_play:1", "payload": "ver=1.0, type=text, body=" + body,
                "to": [{"accountId": account_id, "deviceUniqueId": duid, "platform": "PS5"}]})
            self.assertEqual(status, 204)

        ours = {"sid": 7, "peerSid": 0, "skey": "AA==", "natType": 2, "candidate": [
            {"type": "LOCAL", "addr": "10.0.0.2", "mappedAddr": "0.0.0.0", "port": 5000, "mappedPort": 0}]}
        for offer_req, accept_req in ((2, 3), (5, 6)):  # control, then data channel
            offer = signal()
            self.assertEqual(offer["action"], "OFFER")
            self.assertEqual(len(offer["connRequest"]["candidate"]), 2)
            send("RESULT", offer["reqId"])
            send("OFFER", offer_req, ours)
            self.assertEqual(signal(), {"action": "RESULT", "reqId": offer_req, "error": 0, "connRequest": {}})
            send("ACCEPT", accept_req, ours)
            accept = signal()
            self.assertEqual((accept["action"], accept["connRequest"]["peerSid"]), ("ACCEPT", 7))
            send("RESULT", accept["reqId"])

        self.assertEqual(call("DELETE", f"/api/sessionManager/v1/remotePlaySessions/{session['sessionId']}/members/me")[0], 204)
        _, data = self.request("GET", "/__mock/events")
        kinds = [e["event"] for e in json.loads(data)["events"]]
        self.assertEqual(kinds.count("channel_exchanged"), 2)
        self.assertNotIn("not_mocked", kinds)
        self.assertNotIn(token["access_token"], data.decode())

    def test_events_never_carry_codes_or_tokens(self):
        code = self.sign_in("ok")["code"]
        _, token = self.exchange(code)
        self.list_consoles(token["access_token"])
        response, data = self.request("GET", "/__mock/events")
        text = data.decode()
        for secret in (code, token["access_token"], token["refresh_token"]):
            self.assertNotIn(secret, text)
        self.assertIn("exchange_ok", text)


if __name__ == "__main__":
    unittest.main()
