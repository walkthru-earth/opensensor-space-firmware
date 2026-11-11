# Security policy

## Supported versions

Only the newest commit on `main` and the newest published release get security
fixes. There are no backports to older releases.

## Reporting a vulnerability

Report privately with the "Report a vulnerability" button on the
[Security tab](https://github.com/walkthru-earth/opensensor-space-firmware/security)
of this repository. Never open a public issue for a vulnerability. The
maintainers answer in the private report.

Never include a device PIN, a Wi-Fi password or a LAN token in a report.

## Scope

The policy covers the firmware in `firmware/`, the host tools in `tools/` and
the workflows in `.github/`. Report a problem in third party code to its
upstream project. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists that
code and where it comes from.

## Known limits

These limits are documented and are not new findings. See
[security hardening](docs/shared/security-hardening.md) for the full list.

- Images are not signed for OTA.
- Rollback is off.
- LAN sync is plain TCP.
- There is no flash encryption.
