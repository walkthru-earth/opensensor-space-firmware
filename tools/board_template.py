"""Board template (#85) headers, digest and checks shared by both contract tests.

The host fixture prints the template that tools/board_template.h builds from
the board tables, and the bytes the firmware embeds from telemetry_template.h.
check() fails when the checked-in headers are stale, and write() regenerates
them. run() also writes or checks the published copy in docs/shared/templates
(#86). desc is the SHA-256 of the exact embedded bytes, never a re-serialized
copy. validate() runs the pinned stac-opensensor v0.1.0 schemas and the
profile rules a schema cannot express.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

SCHEMAS = Path(__file__).resolve().parent / "stac_opensensor/v0.1.0"
SPEC = "https://github.com/walkthru-earth/stac-opensensor (a017edf)"
CHUNK = 74


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def c_literal(raw: bytes) -> str:
    text = raw.decode("ascii")
    require(all(0x20 <= ord(c) <= 0x7E for c in text), "template is printable ASCII")
    # Pack escaped characters up to CHUNK columns, never splitting an escape,
    # so each line stays within 80 columns.
    lines, piece = [], ""
    for c in text:
        escaped = "\\" + c if c in '"\\' else c
        if len(piece) + len(escaped) > CHUNK:
            lines.append(f'    "{piece}"')
            piece = ""
        piece += escaped
    lines.append(f'    "{piece}"')
    return "\n".join(lines)


def template_header(raw: bytes, regenerate: str) -> str:
    return f"""#pragma once

// GENERATED, do not edit. Regenerate with
// {regenerate}
// The stac-opensensor v0.1.0 board template (#85), built by
// tools/board_template.h from telemetry_fields.inc, telemetry_semantics.inc,
// telemetry_classes.inc and the board constants in telemetry_contract.h.
// Minified printable ASCII. telemetry_descriptor_digest.h holds the SHA-256
// of exactly these bytes, without the terminating NUL.

namespace telemetry {{
namespace contract {{
// clang-format off
inline constexpr char kTemplateJson[] =
{c_literal(raw)};
// clang-format on
}} // namespace contract
}} // namespace telemetry
"""


def digest_header(raw: bytes, regenerate: str) -> str:
    return f"""#pragma once

namespace telemetry {{
namespace contract {{
// GENERATED, do not edit. Regenerate with
// {regenerate}
// desc, the SHA-256 of kTemplateJson in telemetry_template.h without its NUL.
// Checked by tools/build-idf-board.sh and the host contract test.
constexpr const char *kDescriptorSha256 =
    "{hashlib.sha256(raw).hexdigest()}";
}} // namespace contract
}} // namespace telemetry
"""


def write(firmware: Path, generated: bytes, regenerate: str) -> None:
    (firmware / "telemetry_template.h").write_text(template_header(generated, regenerate))
    (firmware / "telemetry_descriptor_digest.h").write_text(digest_header(generated, regenerate))


def check(firmware: Path, generated: bytes, embedded: bytes, desc: str, regenerate: str) -> str:
    """Fail on a stale header, return desc."""
    stale = f"stale board template, run {regenerate}"
    require((firmware / "telemetry_template.h").read_text() == template_header(generated, regenerate), stale)
    require((firmware / "telemetry_descriptor_digest.h").read_text() == digest_header(generated, regenerate), stale)
    require(embedded == generated, "embedded kTemplateJson differs from the generated template")
    digest = hashlib.sha256(embedded).hexdigest()
    require(desc == digest, "kDescriptorSha256 is not the SHA-256 of kTemplateJson")
    require(json.dumps(json.loads(embedded), separators=(",", ":"), ensure_ascii=True).encode() == embedded,
            "template is minified JSON with no duplicate key")
    return digest


def validator() -> Draft7Validator:
    resources = []
    for name in ("template.json", "profile.json", "schema.json"):
        contents = json.loads((SCHEMAS / name).read_text())
        resources.append((contents["$id"].rstrip("#"), Resource.from_contents(contents)))
    registry = Registry().with_resources(resources)
    schema = json.loads((SCHEMAS / "template.json").read_text())
    return Draft7Validator(schema, registry=registry, format_checker=FormatChecker())


# (type, roles, has unit, aggregate) per stac-opensensor PROFILE at
# 83787768cc8451109d48a03d4ef5493ffcd403dd.
LOCATION_COLUMNS = {
    "h3_cell_id": ("int64", ["referenceInformation"], True, "mode"),
    "h3_resolution": ("int32", ["referenceInformation"], True, "mode"),
    "location_source": ("uint8", ["qualityInformation"], True, "mode"),
    "geometry": ("geometry", ["coordinate"], False, "mode"),
}


def validate(template: dict, dictionary: dict, dictionary_uri: str) -> None:
    errors = sorted(validator().iter_errors(template), key=lambda e: list(e.path))
    require(not errors, "template schema: " + "; ".join(
        f"{'/'.join(map(str, e.path))}: {e.message}" for e in errors[:5]))
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    fields = dictionary["fields"]
    firmware = template["firmware"]
    require(template["id"] == firmware["schema"] == dictionary["schema"], "id is the firmware schema name")
    require(firmware["dict"] == dictionary["sha256"], "firmware.dict is the dictionary digest")
    links = [link for link in template["links"] if link["rel"] == "describedby"]
    require(len(links) == 1 and links[0]["file:checksum"] == "1220" + dictionary["sha256"],
            "describedby checksum is 1220 plus dict")
    # The footer dictionary_uri itself, raw at a pinned commit (#89), so the
    # bytes at the link are the bytes hashed. tools/pinned_uris.py proves the
    # commit and the bytes with git.
    require(re.fullmatch(r"https://raw\.githubusercontent\.com/walkthru-earth/opensensor-space-firmware/[0-9a-f]{40}/.+",
                         dictionary_uri) is not None, "dictionary URI is raw at a full commit")
    require(links[0]["href"] == dictionary_uri, "describedby href is the footer dictionary URI")
    columns = template["item_assets"]["data"]["table:columns"]
    require([c["name"] for c in columns] == [f["name"] for f in fields], "one column per FIELD, in order")
    types = {1: "int32", 2: "int64", 4: "float"}
    # Logical types by firmware unit, as telemetry_logical.h maps them.
    logical = {"uint8_code": "uint8", "wkb_point_crs84": "geometry"}
    for column, field in zip(columns, fields, strict=True):
        want = logical.get(field["unit"]) or types[field["type"]]
        require(column["type"] == want, f"type: {field['name']}")
        require(column["opensensor:field_id"] == field["field_id"], f"field_id: {field['name']}")
        require(column["opensensor:procedure"] == field["procedure"], f"procedure: {field['name']}")
        require(column["opensensor:property"] == field["property"], f"property: {field['name']}")
        unit = column.get("unit", "")
        require(firmware["unit_labels"].get(field["name"], unit) == field["unit"],
                f"unit label: {field['name']}")
        require(field["name"] not in firmware["unit_labels"] or unit != field["unit"],
                f"unit label only where it differs: {field['name']}")
        # stac-opensensor 83787768 PROFILE location columns.
        spelled = LOCATION_COLUMNS.get(field["name"])
        if spelled is not None:
            got = (column["type"], column["roles"], "unit" in column, column["opensensor:aggregate"])
            require(got == spelled, f"location column spelling: {field['name']} {got}")
        gate = column.get("opensensor:valid_when")
        require(gate is None or gate["column"] in {f["name"] for f in fields}, f"gate column: {field['name']}")
        derived = column.get("opensensor:derived")
        require((derived is not None) == (column["roles"] == ["modelResult"]), f"derived: {field['name']}")
        if unit in ("{code}", "{boolean}"):
            classes = column.get("classification:classes", [])
            require([c["value"] for c in classes] == sorted({c["value"] for c in classes}),
                    f"class values ascending and unique: {field['name']}")
    names = {f["name"] for f in fields}
    if "geometry" in names:
        require(template["item_assets"]["data"].get("table:primary_geometry") == "geometry",
                "table:primary_geometry is geometry")
        sources = [c for c in columns if c["name"] == "location_source"]
        require(len(sources) == 1 and [k["value"] for k in sources[0].get("classification:classes", [])]
                == [0, 1, 2, 3], "location_source lists codes 0 to 3")
    else:
        require("table:primary_geometry" not in template["item_assets"]["data"], "no geometry column")
    require(firmware["live_keys"] == {f["name"]: f["live"] for f in fields if f["live"]},
            "live_keys follow the dictionary")
    require(list(firmware["live_keys"]) == [f["name"] for f in fields if f["live"]], "live_keys in field order")
    require(template["instruments"] == list(dict.fromkeys(template["instruments"])), "instruments unique")
    procedures = {f["procedure"] for f in fields}
    require(set(template["instruments"]) <= procedures, "instruments are FIELD procedures")


# C++ comments, then string and character literals, in one pass so a quote
# inside a comment or a comment marker inside a string is never misread.
_CXX_NOISE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
# A designated initializer or a member write through . or ->, with any spacing.
_SAMPLE_INTERVAL = re.compile(r"(?:\.|->)\s*sample_interval_ms\b")
SAMPLE_INTERVAL_CASES = (
    ("const oslogger::Config c{.sample_interval_ms = 30000};", True),
    ("config . sample_interval_ms = 30000;", True),
    ("config->sample_interval_ms = 30000;", True),
    ("// .sample_interval_ms stays the default", False),
    ("/* config.sample_interval_ms */ int x;", False),
    ('const char *k = "{\\".sample_interval_ms\\":1}";', False),
    ("std::uint32_t sample_interval_ms_hint = 1;", False),
)


def sets_sample_interval(source: str) -> bool:
    """True when C++ source names Config::sample_interval_ms as a member, outside comments and literals."""
    code = _CXX_NOISE.sub(lambda match: " " if match.group(0)[0] == "/" else '""', source)
    return _SAMPLE_INTERVAL.search(code) is not None


def run(executable: Path, firmware: Path, dictionary: dict, board_sources: list[Path], write_out: bool,
        regenerate: str) -> str | None:
    """Regenerate with write_out, else check and validate. Return desc."""
    import subprocess

    import published_templates

    # sample_interval_s is the oslogger::Config default. A board that sets its
    # own interval must pass it to the generator first.
    for sample, overrides in SAMPLE_INTERVAL_CASES:
        require(sets_sample_interval(sample) == overrides, f"sample interval scan misread {sample!r}")
    for source in board_sources:
        require(not sets_sample_interval(source.read_text()),
                f"{source.name} overrides the sample interval the template assumes")
    generated = subprocess.check_output([str(executable), "template"])
    if write_out:
        write(firmware, generated, regenerate)
        published_templates.write()
        print(f"WROTE {firmware.name} board template, {len(generated)} bytes, rerun without --write-template",
              flush=True)
        return None
    embedded = subprocess.check_output([str(executable), "embedded"])
    desc = check(firmware, generated, embedded, dictionary["descriptor_sha256"], regenerate)
    validate(json.loads(embedded), dictionary, dictionary["uri"])
    # The published copy (#86) is the same bytes, so it passes the same checks.
    published = published_templates.published_path(firmware, dictionary["sha256"])
    require(published.is_file() and published.read_bytes() == embedded,
            f"stale {published.relative_to(published_templates.ROOT)}, run {regenerate}")
    validate(json.loads(published.read_bytes()), dictionary, dictionary["uri"])
    print(f"PASS board template {len(embedded)} bytes, valid v0.1.0, desc {desc}", flush=True)
    return desc
