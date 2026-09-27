# PLE-741: CI builds and publishes the Oculus Go VR APK

Everything below was checked on CT950 before landing. The CI run on `android-port`, the
`android-latest` download and the Go install can only happen after the branch lands.
They are listed at the end.

## What exists on GitHub (created 2026-09-27)

- `Wnt/pleikkari-android` (private) has deploy key id `164587595`, "Wnt/pleikkari CI: OVR
  SDK fetch (PLE-741, read-only)", ed25519 `SHA256:58l1j8KqKl8UtCS465XiKe+MNbJsVjqPBJqvDT+12as`,
  **read-only**. Its private half exists only as the `Wnt/pleikkari` secret
  `OVR_SDK_DEPLOY_KEY`. The local copy was shredded after the checks below.
- `Wnt/pleikkari-android` stores `ovr-sdk-1.35.0-ci.tar.gz` (sha256
  `7ffff9dfd8d441ffb853ac8488442841cab8b8f0db75bfc535fbff16709cc6fe`, 134794 bytes) as a
  Git LFS object that no commit references. The same bytes, plus `PROVENANCE.txt` and
  `SHA256SUMS`, are on its release `ci-ovr-sdk-1.35.0`.
- The workspace's `docs/OCULUS-GO.md` ("CI build of the Go VR APK") explains how to revoke
  and rotate the key and how to refresh the bundle.

Why an LFS object and not only the release asset: a deploy key is an SSH key. It reaches
git-upload-pack, git-receive-pack (refused for read-only keys) and git-lfs-authenticate,
but never the REST API that serves release assets. The upload/push refusals are in `fetch.txt`.

## Checked locally

- `fetch.txt`: `.github/scripts/fetch-ovr-sdk.py` with the deploy key fetched the bundle,
  matched the pinned sha256 and size, and passed `SHA256SUMS` (12 files). The same key was
  refused an LFS upload token and a `git-receive-pack` ("marked as read only").
- The fetcher also failed cleanly on:
  - a wrong sha256 (LFS 404);
  - a wrong size (LFS 422);
  - an empty secret;
  - an unregistered key ("Permission denied (publickey)");
  - tar members that are `../`, absolute or symlinks;
  - a file that `SHA256SUMS` does not list;
  - a tampered file;
  - a bundle missing a Gradle-required file.
- Every file in the bundle is byte-identical to its member of `ovr_sdk_mobile_1.35.0.zip`,
  and that zip matches PLE-617's `build/sdk/SHA256SUMS`. `ovr-sdk-ci.py bundle` is
  deterministic: two runs gave identical bytes.
- I ran the job's steps by hand:
  1. Link the fetched copy to `third_party/ovr_sdk_mobile/VrApi`.
  2. Run the exact Gradle command (`Go VrApi: SDK enabled (arm64 only)`, 1m29s).
  3. Run `check-go-vr-apk.sh`.
  4. Run the keystore-restore and signer steps, with a scratch HOME.

  `apk-check.txt` shows the check passing on that APK. It fails, with five `::error::`
  lines, on the current flat `android-latest` APK.
- Signer digest: `52717a10c7dd…a01c` for the local build, for the CI-signed flat
  `android-latest` APK and for CT950's `~/.android/debug.keystore`. So the CI keystore is
  the same key, and the CI Go APK will `install -r` over builds from CT950.
- `actionlint` 1.7.12 with shellcheck reports nothing on `build-android.yml`.
- `gate.txt` shows `GATE: PASS`.

## After landing (not done here)

1. The first push to `android-port`: `build-android-go-vr` green, including
   "Check the APK is the Go VR variant". `publish-android-go-vr` green.
2. `gh release download android-latest -R Wnt/pleikkari -p 'pleikkari-android-go-vr.apk'`,
   then `unzip -l` on it shows `lib/arm64-v8a/libpleikkari-vr.so` and `libvrapi.so`.
3. Under `scripts/dev/device.py run --resource go`:
   1. Back up the app data.
   2. `go.sh install` that APK.
   3. `run-as fi.madekivi.pleikkari am start --user 0 -n
      fi.madekivi.pleikkari/.stream.StreamVrActivity --ez vr_cinema_preview true`.
   4. Look for `GoCinema: VrApi cinema entered`.
   5. Run `am force-stop`.

   The Go was held by PLE-715, with PLE-729 and PLE-746 queued, when this ticket finished.
