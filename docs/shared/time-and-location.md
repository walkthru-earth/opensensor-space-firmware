# Time and private station location

Current native ESP-IDF design, 2026-10-09. The reusable implementations are
`firmware/common/connectivity/src/network_time.*` and the optional
[`location` component](../../firmware/common/location/README.md). Boards own
their network lifecycle and RTC hardware; shared code owns policy, validation
and the telemetry contract. No Arduino API is involved.

## Router and offline time first

Sampling remains on the monotonic 10-second schedule. An available RTC or an
authenticated phone `SET_TIME` can supply UTC without internet. Wi-Fi association
does not establish UTC. Until a real anchor exists, UTC fields remain null and
the file belongs to the unanchored boot.

Automatic SNTP tries these sources in order:

1. NTP addresses advertised by the router through DHCP option 42.
2. The two explicitly configured local or approved server addresses.
3. `time.cloudflare.com`, only when public fallback is enabled and earlier
   candidates fail. Public fallback is enabled by default as the owner requested.

A router must actually advertise or run a time service. The firmware does not
assume that the default gateway is an NTP server. DHCP discovery is prepared
before association; a new lease supplies fresh router addresses. Settings
changes alone do not disconnect Wi-Fi. DNS resolution and network responses
run asynchronously, with bounded attempts and generation checks so a late
lookup cannot replace a newer policy. One endpoint is tried at a time. DNS has
a 30-second budget, a resolved endpoint has a 15-second response budget, and
failed cycles retry with a 30-to-300-second backoff. A successful cycle waits
the configured interval, normally 3,600 seconds.

Only a supported, valid SNTP response produces `clock_status=3`. The storage
worker consumes the anchor through its existing clock-epoch queue. No callback
accesses the card, and finalized files are never redated. Losing Wi-Fi keeps
the existing monotonic estimate; the status reports the age of the latest
network synchronization and when that evidence is stale. A board without a
usable RTC starts unanchored again after reboot.

The common Android/iOS session checks current STATUS clock evidence on each
authenticated connection and refreshes unset/RTC clocks, external anchors older
than six hours, or current estimates more than two seconds from the phone. That accepted host anchor can change
`clock_status` back to 1 even after a successful network anchor, until a later
SNTP cycle succeeds. The network service's last-sync evidence describes that
service; it does not mean NTP exclusively owns the active logger clock.

The implementation uses ordinary SNTP, which is unauthenticated. Encrypted BLE
time provisioning remains useful offline; NTP adds automatic availability, and
does not make an authenticity or precision guarantee. Cloudflare explicitly
documents its [public NTP service](https://developers.cloudflare.com/time-services/ntp/).

## Deployment and country policy

Set `ntp.public_fallback=0` to restrict time acquisition to approved DHCP or
configured endpoints. Disable `ntp.dhcp` too when an arbitrary router should
not select the source. DNS country suffixes, country pool names, anycast and
the Thing's declared country are not proof that traffic stays inside a
country. Select deployment-approved endpoints and network policy explicitly.
An offline RTC or authenticated phone time remains available without an
external server. See [configuration keys and pages](ble-sync-protocol.md#device-configuration-v2).

The requested [public server gist](https://gist.github.com/mutin-sa/eea1c396b1e610a2da1e5550d94b0453)
was read in full through GitHub's API on 2026-10-09: revision
`3b49ddfb9d081682f551e3eaf04fc9a1b507d4e6`, its complete 8,642-byte file and
196 comments across two API pages. The web view hides earlier comments. It
provides discovery leads, including national services, operator restrictions,
obsolete entries and leap-smear warnings. It is not an availability monitor,
operator permission list or country-law source. No server directory is copied
into firmware and no country restrictions are inferred from it.

Do not mix leap-smearing providers with unsmeared sources. Google's
[operator guidance](https://developers.google.com/time/smear) explains the
compatibility concern. The default public provider is a single unsmeared
service. Generic `pool.ntp.org` and other vendors' zones are not embedded as
appliance defaults: the [NTP Pool vendor rules](https://www.ntppool.org/en/vendors.html)
require an appliance's own vendor zone.

## Thing location from the phone

Location means the monitoring device or Thing's location. It starts unset.
In the Android/iOS mobile app's Device settings, the owner stands beside the
sensor, chooses a resolution and taps **Use phone location**. Permission and a
foreground one-shot fix are requested only from this tap. The phone converts
the fix to H3 before it enters UI state or the device command. Exact phone
coordinates are never persisted, logged or sent to firmware. **Save on device**
sends only a cell, maximum resolution and optional declared country; the app
reads page 1 back before reporting confirmation. This does not enable uploads.
Older firmware without that page shows setup as unsupported.

The default maximum H3 resolution is 8 (#83), configurable from 0 through 15.
Lower resolutions disclose larger areas. Resolution 8 averages about 0.74 km²;
actual area varies, and an approximate phone fix does not become accurate by
choosing a finer cell. The official [H3 resolution table](https://h3geo.org/docs/core-library/restable/)
describes these averages. Fine cells can identify a specific place. Public
sharing requires its own disclosure policy.

Firmware validates the cell using pinned official H3 v4.5.0 and coarsens it to
the configured maximum before persistence. A supplied coarser cell stays
coarser. Changing the maximum to a finer value cannot recover discarded
location; the phone requires a new fix to create a finer candidate. The
public latitude/longitude are always the [H3 grid center](https://h3geo.org/docs/api/indexing/),
derived from the retained cell, not the original fix or a claimed device point.

Optional `location.country` is an owner-declared assigned ISO 3166-1 alpha-2
code, such as `EG`. H3 does not provide administrative country lookup. A cell
may span a border and its center cannot prove a country's assignment.

### Setup needed

A fixed Thing with no `location.cell` needs setup (#147). The board derives
this from the location settings every time it reads them, and stores no
extra key. A moving Thing never needs setup. While setup is needed:

- The advert sets flag bit 6 `setup`, so any phone in range can offer the
  setup wizard without connecting first. `pixi run ble-sync scan` prints it.
- GET_CONFIG page 1 reports `"thing":{"mobility":"fixed","setup":1}`.
- The CoreS3 screen shows `Setup needed, open the OpenSensor app` in the
  footer of every page. Waveshare V2 has no screen.

A `SET_CONFIG` that sets a cell or `thing.mobility=moving` clears the bit at
once and starts the 60 second fast advertising window, so a scanning phone sees
the change within a minute. The screen line clears on its next redraw, at most
10 seconds later. Clearing the cell raises the bit again.

Setup never blocks logging. Rows keep four location nulls until a cell is set,
and a cell change splits the open file as any location change does.

### GNSS cell readback

On a board with a receiver, GET_CONFIG page 1 also carries
`location.gnss_cell`. It lets the setup phone show the cell the board's own
receiver sees next to the map picker. The value is:

- Always an H3 resolution 8 cell, whatever `location.resolution` is.
- `""` without a fix from the last 15 seconds, the same freshness rule as
  moving rows.
- `""` when `location.resolution` is below 8, because the reader never keeps
  a cell finer than the owner cap and a cell cannot be upsampled.
- Never a coordinate, never logged and never stored. It is read through the
  same board-neutral `oslogger::Hooks::gnss_fix` hook that moving rows use.

The key is omitted on a board without a GNSS hook, CoreS3 today. Waveshare V2
sends it from firmware v2.18. The phone never copies it into `location.cell`
on its own. The owner confirms the cell on the map first.

## Parquet provenance

From the Thing release (#83) both boards hold four nullable location columns
next to each other:

| Column | Physical type | Meaning |
| --- | --- | --- |
| `h3_cell_id` | INT64 | Valid retained H3 cell, stored as a numeric index |
| `h3_resolution` | INT32 | Actual cell resolution, which may be coarser than the maximum |
| `location_source` | UINT8 | 0 none, 1 configured, 2 gnss, 3 phone. A fixed Thing writes 1, a moving Thing with a GNSS hook writes 2. Nothing writes 3 yet |
| `geometry` | GEOMETRY | 2D WKB Point at the cell centre, no crs (OGC:CRS84), with GeoParquet 2.0 `geo` footer metadata |

Unset or invalid location produces four nulls, and `geometry` is null exactly
when `h3_cell_id` is null. Each sample captures its
location settings before entering the worker queue. A cell, precision cap or
country change splits the batch/open file, so a later setting never relabels
buffered readings. The footer carries no location since #88. The rows hold
the cell, resolution, source and centre point, and the country is not written to the file.
The owner also sets `thing.mobility` (`fixed` by default, or `moving`), which
the footer `thing.mobility` records per file.
Existing archives are immutable.

## Moving Things and GNSS

`thing.mobility` picks the source of each row, one row at a time.

| Mobility | Fresh GNSS fix | Row location |
| --- | --- | --- |
| `fixed` | any | The configured cell, source 1. A receiver never changes it |
| `moving` | yes | The GNSS cell coarsened to the row's `location.resolution`, source 2 |
| `moving` | no | Four nulls, `location_source` null too. Never the configured cell and never an older fix |

A fix is fresh for 15 seconds after the reply was read. Polls are about 12
seconds apart (a 10 second delay plus up to 2 seconds for the reply), so one
silent poll already ages the fix past 15 seconds and rows about 15 to 22
seconds after the last fix are null. A reply with no fix clears it at once. A moving Thing without a GNSS hook, CoreS3 today,
writes nulls on every row.

Boards that carry a receiver opt in through `oslogger::Hooks::gnss_fix` and
the board-neutral `firmware/common/gnss` and `firmware/common/gnss_reader`
components (see [board porting](board-porting.md)). The receiver position
becomes an H3 cell at the owner resolution the moment it is parsed. The raw
latitude and longitude never reach a log, the card, BLE, LAN or the LIVE view,
and the firmware keeps only the cell, its resolution and the fix monotonic
time. Waveshare V2 is the first board with the hook, from firmware v2.17.

A GNSS cell change never splits a batch or file, because the cell is not part
of the location snapshot that `fits_batch` compares. A file from a moving
Thing holds many cells, each row with its own. A change of
`thing.mobility`, the configured cell, the resolution cap or the country still
splits as before.

GNSS UTC is not a clock anchor. A fix does not set UTC, so rows keep null UTC
until a host anchor, the RTC or NTP arrives. Using GNSS time as an anchor is
follow-up work on #83.

### Reserved `location.phone_cell`

The reserved `SET_CONFIG` key `location.phone_cell` is the future source 3
path, a cell from the phone's own fix that the board keeps in RAM only and
never persists. This firmware does not accept it yet. Its planned split and
fallback rules live in the reserved row of the
[SET_CONFIG table](ble-sync-protocol.md) and are not GNSS rules. GNSS does not
depend on it. A GNSS cell change never splits a file, and a moving Thing with
no fresh fix writes nulls with no fallback to a phone cell, the configured
cell or an older fix.

Host tests and native builds verify contracts and compilation. Router option
42, phone permissions/fixes, RTC writes, radio coexistence and physical card
durability still require dated device evidence.
