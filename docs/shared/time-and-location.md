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
sensor and taps **Use phone location**. Permission and a
foreground one-shot fix are requested only from this tap. The phone converts
the fix to H3 before it enters UI state or the device command. Exact phone
coordinates are never persisted, logged or sent to firmware. **Save on device**
sends only a cell and optional declared country, and the app
reads page 1 back before reporting confirmation. This does not enable uploads.
Older firmware without that page shows setup as unsupported.

Every published cell is H3 resolution 8, for fixed and moving Things alike
(OD-2026-11-04-B, #139). Resolution 8 averages about 0.74 km², actual area
varies, and an approximate phone fix does not become accurate by choosing a
finer cell. The official [H3 resolution table](https://h3geo.org/docs/core-library/restable/)
describes these averages. The STAC profile gives a file no Item when a cell is
not resolution 8, so the board never writes another resolution.

`location.resolution` is fixed at 8 and kept so a client may restate it.
`SET_CONFIG` refuses any other value, such as 0, 7, 9 or 15, with `ERROR` 12
and detail `location.resolution`, and nothing changes. A stored record from
older firmware with another value keeps its bytes in NVS. At boot the board
reads it as 8 and prints one serial line, for example
`CONFIG location.resolution=7 reset to 8`. The record is rewritten with 8 only
by the next save that writes it. The one older exception is a boot that
migrates a legacy factory PIN, which already saves the whole config during
boot and so writes 8 at that point.

Firmware validates the cell using pinned official H3 v4.5.0 and coarsens a
finer cell to its resolution 8 parent before persistence. A cell coarser than
resolution 8 cannot become one, so `SET_CONFIG` refuses it with detail
`location.cell`. A stored cell coarser than resolution 8 boots as unset and
logs the invalid stored profile line. The
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

- Always an H3 resolution 8 cell.
- `""` without a fix from the last 15 seconds, the same freshness rule as
  moving rows.
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
| `h3_resolution` | INT32 | Resolution of the row's cell, 8 in every located row (#139) |
| `location_source` | UINT8 | 0 none, 1 configured, 2 gnss, 3 phone. A fixed Thing writes 1. A moving Thing writes 3 while `location.phone_cell` is set (#140), else 2 with a GNSS hook and a fresh fix |
| `geometry` | GEOMETRY | 2D WKB Point at the cell centre, no crs (OGC:CRS84), with GeoParquet 2.0 `geo` footer metadata |

Unset or invalid location produces four nulls, and `geometry` is null exactly
when `h3_cell_id` is null. Each sample captures its
location settings before entering the worker queue. A cell or
country change splits the batch/open file, so a later setting never relabels
buffered readings. The footer carries no location since #88. The rows hold
the cell, resolution, source and centre point, and the country is not written to the file.
The owner also sets `thing.mobility` (`fixed` by default, or `moving`), which
the footer `thing.mobility` records per file.
Existing archives are immutable.

## Moving Things and GNSS

`thing.mobility` picks the source of each row, one row at a time. A phone
cell labels moving rows only (see the next section).

| Mobility | Phone cell | Fresh GNSS fix | Row location |
| --- | --- | --- | --- |
| `fixed` | any | any | The configured cell, source 1. Neither a phone cell nor a receiver changes it |
| `moving` | set | any | The phone cell at resolution 8, source 3 |
| `moving` | unset | yes | The GNSS cell at resolution 8, source 2 |
| `moving` | unset | no | Four nulls, `location_source` null too. Never the configured cell and never an older fix |

A fix is fresh for 15 seconds after the reply was read. Polls are about 12
seconds apart (a 10 second delay plus up to 2 seconds for the reply), so one
silent poll already ages the fix past 15 seconds and rows about 15 to 22
seconds after the last fix are null. A reply with no fix clears it at once. A moving Thing without a GNSS hook, CoreS3 today,
writes nulls on every row.

Boards that carry a receiver opt in through `oslogger::Hooks::gnss_fix` and
the board-neutral `firmware/common/gnss` and `firmware/common/gnss_reader`
components (see [board porting](board-porting.md)). The receiver position
becomes an H3 resolution 8 cell the moment it is parsed. The raw
latitude and longitude never reach a log, the card, BLE, LAN or the LIVE view,
and the firmware keeps only the cell, its resolution and the fix monotonic
time. No finer cell is ever computed, so finer GNSS detail stays in the RAM
of the reply parser only. Waveshare V2 is the first board with the hook, from firmware v2.17.

A GNSS cell change never splits a batch or file, because the cell is not part
of the location snapshot that `fits_batch` compares. A file from a moving
Thing holds many cells, each row with its own. A change of
`thing.mobility`, the configured cell or the country still
splits as before. A moving Thing does not split on a phone cell change
either.

GNSS UTC is not a clock anchor. A fix does not set UTC, so rows keep null UTC
until a host anchor, the RTC or NTP arrives. Using GNSS time as an anchor is
follow-up work on #83.

### Phone cell, `location.phone_cell`

From CoreS3 v6.28 and Waveshare V2 v2.22 the board accepts the `SET_CONFIG`
key `location.phone_cell` (#140). It is the source 3 path, the H3 cell of the
phone's own fix. The phone sends a cell at resolution 8 or finer and never
sends coordinates. The board keeps the resolution 8 parent through the same
`published_cell()` rule as `location.cell`. A coarser cell or a value that is
not a valid cell answers `ERROR` 12 with detail `location.phone_cell`, and
nothing changes. An empty value clears it.

The cell lives in RAM only. The phone cell is never stored, the `oscfg/cfg`
record has no field for it, and a reboot clears it. A request that holds only
this key still rewrites a record an earlier failed save left unsure, without
the phone cell. GET_CONFIG page 1 reports it as `location.phone_cell`, so the
app can tell when a reboot or the age limit cleared it.

From CoreS3 v6.32 and Waveshare V2 v2.25 the cell has an age limit (owner
decision 2026-11-09). The board drops it 300 seconds
(`config::kPhoneCellMaxAgeUs`) after the last accepted `SET_CONFIG` that
carried the key. A new value and the same value again both restart the age, a
request without the key does not, and a refused request changes nothing. A BLE
or LAN disconnect does not clear it, so both links behave the same. The age
runs on the monotonic `esp_timer_get_time()` clock, never UTC, so a missing or
changed clock anchor does not move it. A cell is kept for ages below 300 s
and dropped at 300 s exactly. Rows come every 10 s, so the last phone row is
sampled before the 300 s mark and the next one falls back.

Expiry clears the RAM value as an empty value would. Every reader of the
settings checks the age under the settings lock before it copies them, so the
row snapshot, GET_CONFIG and the next `SET_CONFIG` all see the same cell.
Expiry writes no NVS, marks no record for a write, logs no line and never
splits a file. It is not a request, so it produces no `SET_CONFIG` reply or
action. On a moving Thing a later request that sends the cell again sets
`Actions::location_changed`, the same as a first set. No firmware reads that
flag today, only the host tests do, so it changes nothing on the device. The
app refreshes the cell about once a minute while it is connected, plus once on
connect and whenever page 1 shows it empty (opensensor-space-mobile #264).

A moving Thing writes it in every row with `location_source` 3 while it is
set, ahead of a fresh GNSS fix. When it is cleared or reaches its age
limit, rows fall back to the table above. A fixed Thing accepts and reports
it but ignores it for rows, as it ignores a receiver. Its rows keep the configured cell with
`location_source` 1, as the stac-opensensor profile requires, and the owner
sets that cell with the map picker. A fixed Thing with only a phone cell and
no `location.cell` writes nulls and keeps `setup` 1.

A phone cell never splits a file. A fixed Thing never uses it, and one moving
file holds many cells.

The board holds and reports only the res 8 cell. The phone fix itself never
reaches the board.

Host tests and native builds verify contracts and compilation. Router option
42, phone permissions/fixes, RTC writes, radio coexistence and physical card
durability still require dated device evidence.
