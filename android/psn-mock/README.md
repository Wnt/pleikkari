# PSN sign-in mock (PLE-284)

A small stand-in for Sony's private Remote Play sign-in, so that onboarding can be
tested on the emulator or a phone repeatedly, unattended, and without a real Sony
account. Every code and token it issues is fake by construction, and none is ever
logged.

It runs behind the [forwarder](https://github.com/Wnt/forwarder) on two public
hosts that have real Let's Encrypt certificates. Nothing needs a custom CA, and
WebAuthn gets a genuine secure origin:

| Host | `/.well-known/assetlinks.json` | Stands for |
|---|---|---|
| `pleikkari-psn.lab.madekivi.fi` | served (`fi.madekivi.pleikkari.psnmock`, and the pre-PLE-568 `com.metallic.chiaki.psnmock`, debug key `52:71:7A…A0:1C`) | a **verified** app link |
| `pleikkari-psn-nolink.lab.madekivi.fi` | 404 | an **unverifiable** link: what `remoteplay.dl.playstation.net` is in production |

## Run it

On CT950 the mock is a systemd user service, `pleikkari-psn-mock.service`. It runs
`serve.sh run` from the shared clone (`/home/wnt/gta6/chiaki-ng`), so it survives
reboots and the garbage collection of any ticket worktree. Landing a change to the mock
in the shared clone takes effect on the next restart.

```bash
systemctl --user restart pleikkari-psn-mock    # restart it, e.g. after a landed mock change
journalctl --user -u pleikkari-psn-mock        # its output
android/psn-mock/serve.sh status               # the unit, and assetlinks HTTP status on both hosts
android/psn-mock/serve.sh install              # (re)install the unit; stops a mock started by hand first
```

Without the unit, for a mock you are changing in a worktree (stop the unit first, both use port 18284):

```bash
android/psn-mock/serve.sh start     # mock on 127.0.0.1:18284 + forwarder-agent, both backgrounded
android/psn-mock/serve.sh stop
```

`serve.sh` fetches `forwarder-agent` from the Wnt/forwarder CI artifact into
`~/.cache/pleikkari/forwarder/` if it is missing. It reads the agent token from
`$FORWARDER_AGENT_TOKEN`, or else from the first env file it finds:
`$PSN_MOCK_AGENT_ENV`, `~/.config/pleikkari/forwarder-agent.env`, then
`~/.config/locator-kiosk/forwarder-agent.env`. The token reaches the agent
through the environment, never on a command line. Logs and stored passkeys live
in `~/.local/state/pleikkari-psn-mock/`.

The mock's own tests are `python3 -m unittest discover -s android/psn-mock` (about 7 s, no network).

## Endpoints

These are the same paths as Sony's, so the app only swaps the host (`PsnServiceEndpoints.mock`):

| Path | Mirrors |
|---|---|
| `GET /2.0/oauth/authorize` | Sony's sign-in page. It checks `response_type`, `client_id` and a registered `redirect_uri`, then offers a password form, *Sign in with a passkey*, *Create a passkey* and *Cancel* |
| `GET /remoteplay/redirect?code=…` | the redirect that carries the code. When no app takes the link, the page is blank, like Sony's |
| `POST /2.0/oauth/token` | code exchange and refresh. Needs the app's Basic client auth; codes are single-use and bound to their `redirect_uri` |
| `GET /2.0/oauth/token/{access}` | token info with `user_id`; the token response itself has none, like Sony's |
| `GET /api/cloudAssistedNavigation/v2/users/me/clients` | the console list: one PS5 with `remotePlay`, and one without it (filtered out) |
| `GET /np/serveraddr` | the push server lookup; answers with the mock's own host (PLE-321) |
| `GET /np/pushNotification` (WebSocket) | the push socket. Every notification below goes to the account's open sockets |
| `POST /api/sessionManager/v1/remotePlaySessions` | session creation; pushes `remotePlaySession:created` and the client's `members:created` |
| `POST /api/cloudAssistedNavigation/v2/users/me/commands` | the console wake. A mock console joins (`members:created` with the duid), posts `customData1` and OFFERs |
| `POST /api/sessionManager/v1/remotePlaySessions/{id}/sessionMessage` | signalling. The console RESULTs the app's OFFER, answers its ACCEPT with an ACCEPT, and after the app RESULTs that, OFFERs again for the data channel |
| `DELETE /api/sessionManager/v1/remotePlaySessions/{id}/members/me` | leaving the session |
| `GET /__mock/events?since=N` | what happened, for tests. Account names only |

Anything else returns `501 not mocked`. The console's candidates are documentation addresses
(192.0.2.64, 198.51.100.64), so the UDP punch after the exchange always times out: the mock proves
the signalling order, not a connection. Whether the forwarder in front passes a WebSocket upgrade
has not been tried.

## Scenarios

The **account name** picks the scenario, so parallel runs never share state. The
part before `+` or `@` is the name: `consoles-500+run7@mock` behaves as
`consoles-500`. Any password works except `wrong`, which is rejected on the page.

| Account | Behaviour |
|---|---|
| `ok` (or any other name) | signs in; the console list has one PS5 |
| `expired-code` | the code has expired before it is exchanged (`400 invalid_grant`) |
| `code-used` | the code counts as already exchanged (`400 invalid_grant`) |
| `cancel` | redirects with `error=access_denied` and no code; the *Cancel* button does the same |
| `network-drop` | the exchange stalls 20 s (past the app's 15 s timeout), then the connection closes |
| `token-refused` | sign-in works; every later refresh is refused (`400 invalid_grant`) |
| `empty-consoles` | sign-in works; the console list is empty |
| `consoles-500` | sign-in works; the console list fails with HTTP 500 |

Passkeys are real WebAuthn (ES256/RS256, discoverable credentials). A passkey is
bound to the host it was created on. The account it was created for picks the
scenario. Attestation statements are accepted without checking.

## The app build

```bash
cd android && ./gradlew -PchiakiPsnMock=verified assembleDebug   # or =nolink
```

- The build is a **separate app**, `fi.madekivi.pleikkari.psnmock` (`com.metallic.chiaki.psnmock` before PLE-568), labelled *PSN MOCK Chiaki*, with a red *PSN MOCK · host* strip over the status bar of every activity. It can never overwrite a real install's PSN credentials or PS5 registration.
- Both mock hosts are declared as links. The verified host is `autoVerify`; the nolink host is declared without it, exactly like Sony's redirect host in `src/main`.
- **It cannot ship.** The mock host comes from `BuildConfig.PSN_MOCK_HOST`, which only the debug build type defines. The release source set's `psnMockHost()` returns `null` and has no mock code at all. A release task with `-PchiakiPsnMock` fails the build.
- The asset link names the debug key in `~/.android/debug.keystore`. A build signed with another key (`chiakiKeystore` in `local.properties`) cannot verify.

## The onboarding test

```bash
android/psn-mock/onboarding_test.py --build                          # verified link, happy path
android/psn-mock/onboarding_test.py --build --link nolink            # production's situation
android/psn-mock/onboarding_test.py --link nolink --select-domain    # the user enabled the link
android/psn-mock/onboarding_test.py --scenario network-drop          # any scenario above
node android/psn-mock/passkey_check.mjs                              # passkey ceremony, headless Chrome
```

The driver needs the mock up and the emulator booted (`scripts/dev/emu.sh start`),
and it takes the emulator reservation. `--serial` runs it on a phone instead; wrap that
in `scripts/dev/device.py run <ticket> --`. An APK already installed with the same hash is
not sent again, because a 20 MB install over Wi-Fi ADB can take minutes. It resets the mock app to a first-run
state by clearing the app's own data with `run-as`, because the adb wrapper
refuses every uninstall. It taps through onboarding and fills the mock's form in
the browser. It fails on:

- any other app in front, above all Android Settings,
- any Settings activity resumed at all, read from the `wm_set_resumed_activity` event log, so a Settings screen that comes and goes between two dumps still fails,
- an app `TextView` of 14 or more words (an instruction paragraph),
- 40 s with no screen change short of an end state (a dead end),
- an app crash,
- for `ok`, the console list never showing *PS5 mock*.

A failure scenario passes when the app settles on one of its own screens with a
control. Artifacts go to `build/psn-mock/onboarding-*/` in the workspace:
`summary.json`, a screenshot and UI dump per distinct screen, and the mock's
events. One run takes 30–90 s.

The driver declines the browser's own prompts the way a user would: Firefox's save-password
sheet, and Android's autofill save sheet (Samsung Pass on the S22, package `android`).

## Proving the driver is a guard: the suite

```bash
android/psn-mock/onboarding_suite.py --build                        # emulator, about 12 min
scripts/dev/device.py run PLE-N -- android/psn-mock/onboarding_suite.py --serial <phone>
```

This is the one command to run after any change to sign-in or onboarding. It is not a
gate step: it needs the emulator or a phone, and takes minutes. Every case uses the nolink
host with the app's link selection explicitly disabled, as production is for a user:

| Case | Expected |
|---|---|
| `clean` | PASS: tab, password, Finish sign-in, console list, Play fails honestly with a live Retry |
| `redirect-dead-end` | FAIL `dead end: … in the browser`: Finish sign-in does nothing, the user is stuck on the blank redirect page |
| `settings-redirect` | FAIL `left the app for com.android.settings (Android Settings)`: returning from the tab opens the link settings |
| `instruction-paragraph` | FAIL `instruction paragraph: …` on the sign-in screen |
| `exit-x` | PASS: on the redirect page the user presses the tab's close button instead of Finish; the app reopens the tab, which the mock's session cookie takes straight back to a fresh redirect (PLE-323) |
| `exit-back` | PASS: the same with the back key, pressed until the tab closes |
| `exit-open-in-browser` | PASS: the tab menu's "Open in <browser>", then the app from its launcher icon. Chrome 133 on the emulator has no such item (only a "Running in Chrome" footer that does nothing), so there the driver emulates it: close the tab, open the redirect page in the full browser, return by the launcher icon (`open_in_browser_item` in summary.json says which ran) |
| `exit-idle` | PASS: the redirect page left alone for 60 s, then Finish sign-in |
| `exit-loses-code` | FAIL `dead end: … in com.metallic.chiaki.psnmock`: leaving the tab strands the user on Continue signing in, as before PLE-323 |
| `clean-after-faults` | PASS again, so no fault leaks into the next run |

A fault case only counts when the driver fails for *that* reason. The faults live in
`app/src/debug/.../PsnMockFault.kt`, run only in a `-PchiakiPsnMock` build, and are
switched per run with the device property `debug.pleikkari.psnmock.fault`, which the
driver sets from `--fault` (and resets to `none` otherwise), so one APK serves every case.

## Proving a release build cannot reach the mock

```bash
android/psn-mock/release_check.py --build --control build/psn-mock/psnmock-verified.apk   # about 1.5 min
```

It checks that Gradle refuses `-PchiakiPsnMock` for a release task, then builds `assembleRelease`
and scans every entry of the release APK (dex, manifest, resources, native libraries), as UTF-8 and
UTF-16LE, for the mock's markers (`lab.madekivi.fi`, `pleikkari-psn`, `psnmock`, `PSN MOCK`, `__mock`).
It also requires the package to be `fi.madekivi.pleikkari`, and requires Sony's production sign-in host
to be found, so an empty scan cannot pass. `--control` scans a mock debug APK and requires the markers
to be found there, which shows the scan would catch a mock build.

## Passkeys in the app's own WebView: the probe (PLE-324)

Since androidx.webkit 1.12, `WebSettingsCompat.setWebAuthenticationSupport(settings, WEB_AUTHENTICATION_SUPPORT_FOR_BROWSER)`
lets a WebView make WebAuthn calls for any origin. If the app could use it, sign-in could stay in the
app's WebView, which sees the redirect, and no tap on the tab would be needed. **It does not work for
this app.** The WebView itself does not stop it: Chromium only records whether the app holds
`CREDENTIAL_MANAGER_SET_ORIGIN`, a normal permission. The providers stop it. They only serve an asserted
origin to an allowlisted browser, which they identify by package and signing certificate:

| Provider (S25, SM-S938B, Android 16, WebView 152.0.7977.88) | Result for `com.metallic.chiaki.psnmock` |
|---|---|
| Google Password Manager, including hybrid (phone as a security key) | `[28442] Invalid calling package`. GMS logs `rejecting asserted origin from app … did not match the privileged allowlist` (the list is `https://www.gstatic.com/gpm-passkeys-privileged-apps/apps.json`; Google approves additions through a request form) |
| Samsung Pass | shows its sheet and the fingerprint prompt, then `CreatePasskeyActivity: not privileged browser` |

The page receives `NotAllowedError`. Nothing is saved. Before Android 14, a browser-mode WebView uses
GMS's FIDO2 browser API instead. That API is also reserved for approved browsers, but no phone below Android 14 was tested.
The feature needs WebView M124 or later (Chromium 916b2555, 2024-03-07), found with
`WebViewFeature.isFeatureSupported(WEB_AUTHENTICATION)`. The raw captures are in
`build/captures/ple324/` in the workspace.

A `-PchiakiPsnMock` build has the probe activity, so this can be re-checked when a provider or WebView changes:

```bash
adb shell am start -n fi.madekivi.pleikkari.psnmock/fi.madekivi.pleikkari.regist.PsnMockWebAuthnProbe \
    --es mode browser --es target mock      # mode: browser|app|none; target: mock|sony
adb logcat -s PsnWebAuthnProbe CredentialManager
```

It logs feature support, the WebView package, the permission, each WebAuthn call and its exact
outcome, and whether the redirect was captured. It never exchanges or logs the code.

## What the mock cannot reproduce about Sony

This list is where the next surprise will come from.

1. **Sony's page is a multi-step JavaScript app. The mock's is a single form.** Sony asks for the ID and the password on separate steps. It can add 2-step verification, captcha or bot checks, "trust this browser", consent and age screens, account-locked and region errors. The mock models none of these.
2. **How Sony redirects.** The mock answers the password form with a plain `302` straight after the user's tap. That is the navigation Chrome most readily hands to an app link. Sony's redirect may come from script, after asynchronous steps and after the tap's user activation has expired. Chrome may then show the blank redirect page even for a link it would otherwise open in the app. The mock's passkey branch does navigate from script (`location.assign`), which is the closer model.
3. **Passkeys.** On the mock, the relying party is the mock host. On Sony it is Sony's domain, whose assetlinks list only Sony's apps. Credential Manager in the app's WebView can never use a Sony passkey. The mock deliberately does not delegate (`handle_all_urls` only). Still, no Android passkey provider was exercised here: the emulator has no Google account, and the check uses a CDP virtual authenticator in desktop Chrome. On the S25, PLE-324 did run both of its providers against the mock from the app's own WebView (see below), and both refused the app. Since PLE-312 a passkey sign-in happens in the same browser tab as a password one and returns through the same Finish sign-in action, which the driver does exercise; the driver itself never performs a passkey ceremony.
4. **The nolink build still carries one verified link.** The mock app declares both hosts, so Android's link settings show "1 verified link". Production has none. What the app does is the same, but the Settings screen text differs.
5. **A network drop is only a stall.** Through Caddy and the forwarder, the app sees a 20 s stall and a closed connection or a `502`. It does not see a TCP reset, DNS failure, airplane mode, captive portal or a network switch mid-exchange.
6. **Error bodies, code lifetime and cancel are guesses.** The `invalid_grant` body and `error_code` numbers are unverified against Sony. Sony's real code lifetime is unknown; the mock uses 300 s. What Sony sends back on cancel is also unknown; the mock uses OAuth's `error=access_denied`.
7. **No sessions or SSO.** Sony may remember a signed-in browser. With `prompt=always` it should still ask, but the mock always shows the form and ignores `duid`, `smcid`, `ui`, `layout_type` and locale.
8. **Token lifetime and refresh.** The mock's refresh tokens never expire and accept any scope. Sony rotates and expires them.
9. **Anything after the console list.** Push WebSocket, session creation, wake and remote play commands return `501`. A mock console cannot be registered or streamed.
10. **Browsers.** The emulator has Chrome only. The S22's default browser is Firefox (checked 2026-09-16), with Samsung Pass as its autofill service; the suite passes there. Samsung Internet is untested, and handles Custom Tabs and app links differently (see PLE-279 on Firefox's blocked background activity start).
