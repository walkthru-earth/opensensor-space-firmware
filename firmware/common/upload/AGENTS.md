# Shared uploader (#81)

- Software only. Off by default through `CONFIG_OS_UPLOAD_ENABLE`. No task,
  no provisioning and no call to source.coop happens from this component
  until #144 wires a `Session` into the storage worker. Each board logger
  component REQUIREs `upload`, so it compiles but never runs.
- Never put a real or realistic `sck_` key, PSK, PIN or token in code,
  tests, logs or docs. Use `<sck key>` style placeholders and the public AWS
  example keys. The API key is only ever sent in the STS POST body, never in
  a URL, header or log line.
- Pure modules (`sigv4`, `sts`, `policy`, `ledger`, `session`, `text`) have no IO,
  heap, clocks or board names. `os_upload_device.*` is the only ESP-IDF
  code, a PSA SHA-256 and an esp_http_client transport with the cert bundle.
- Policy rules are load bearing, each one has a host test and a mutation
  proof. Finalized dated files only, oldest first, one per step. No upload
  until UTC comes from Host or Network, never from the RTC or a Wi-Fi link
  alone. Back off on transient errors. Stop on 401, 403 and rejected
  requests until the key generation changes. A dev target never writes
  outside `<product>/dev/opensensor-space-firmware/<board>/`.
- The ledger is append only. Never rewrite, truncate or delete it, and never
  delete, rename or relabel an archive file because of an upload. The
  object key is `<product>/` plus the prefix plus the SD relative name
  verbatim, built only by `object_key()` and checked again by
  `key_within_target()` before signing.
- The dev prefix lives in one constant, `kDevPrefix` in `os_upload_policy.h`.
- Host gate is `pixi run upload-test` with ASan and UBSan. The SigV4 vectors
  come from aws-c-auth under Apache-2.0, keep their attribution in the test
  and in THIRD_PARTY_NOTICES.md section 6. FakeCloud pins the full PUT
  signature, computed by a separate Python SigV4.
- The transport scratch is static. `ScratchLease` allows one request at a
  time and wipes every buffer on every exit. The STS body holds the key, so
  never read or log the scratch outside a lease.
- esp_http_client copies header values, the session token and the
  Authorization header among them, to its own heap and frees them without
  zeroing. That is outside our code, so never claim full zeroing on device.
- `PolicyState` is RAM only. A reboot clears a stop, so a revoked key gets
  one STS attempt per boot.
