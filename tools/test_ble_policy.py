"""Check BLE notify credits, stall clock, advert window, interval mode, link limits and link auth gates.

Host checks of the pure link policy only; no radio, host stack or central behavior
is established.
Run: pixi run ble-policy-test
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def definition(source: str, name: str) -> re.Match[str] | None:
    """The top level definition of `name`, up to its opening brace. A line
    comment never starts one. Qualifiers between the parameters and the brace
    (noexcept, const, a trailing return type) still match."""
    return re.search(rf"\n(?!/)\S[^\n;]*\b{name}\([^;{{]*\)[^;{{}}()]*{{", source)


def gate_body(source: str, name: str) -> str:
    """The body of the ble_sync.cpp function `name`, by brace matching."""
    match = definition(source, name)
    assert match, f"no definition of {name}"
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(f"unbalanced {name}")


def check_gate_wiring(source: str) -> None:
    """Issue #51. Every response gate asks may_send_frame(), which falls back
    to NimBLE's live state. Refusing on the cached flag alone fails here."""
    body = gate_body(source, "notify_on_host")
    assert "may_send_frame(" in body, "notify_on_host must gate with may_send_frame"
    assert "authenticated.load()" not in body, "notify_on_host must not gate on the cached flag"
    for name in ("drain_responses", "enqueue_response"):
        body = gate_body(source, name)
        assert "response_gate(" in body, f"{name} must gate with response_gate"
        assert "authenticated.load()" not in body, f"{name} must not gate on the cached flag"
    body = gate_body(source, "response_gate")
    assert "may_send_frame(" in body and "subscribed[Response]" in body, body
    assert "BLE REFUSE reason=" in body and "refusal_is_fault(" in body, body
    # Issue #178. os-lan calls try_send_response, which never waits, so its
    # refusal line skips the console sink and stays in LOG_TAIL.
    assert "may_wait ? oslog : oslog.record_only()" in body, body
    # The no-wait path never reaches secure(), which takes the host lock.
    flat = " ".join(body.split())
    assert "may_wait ? response_refusal(" in flat, body
    assert ": response_refusal_now(" in flat, body
    body = gate_body(source, "enqueue_response")
    assert "refusal == GateRefusal::Deferred" in body, body
    assert "return ossync::EnqueueResult::Full;" in body, body
    body = gate_body(source, "try_send_response")
    assert "enqueue_response(frame, length, session, false)" in body, body
    assert "xSemaphoreTake(response_mutex, 0)" in body, body
    body = gate_body(source, "may_send_frame")
    assert "may_send(authenticated.load()" in body and "secure(handle)" in body, body


def case_body(source: str, event: str) -> str:
    """The gap_event case for `event`, up to the next case or default label."""
    start = source.index(f"case {event}:")
    end = re.compile(r"\n  (case BLE_GAP_EVENT_|default:)").search(source, start + 1)
    assert end, f"no label after {event}"
    return source[start:end.start()]


def check_early_subscribe_wiring(source: str) -> None:
    """Issue #51, bonded reconnect. NimBLE can restore a bonded CCCD before it
    reports CONNECT. SUBSCRIBE must hold it and CONNECT must apply it after it
    clears the old link's state, through the same path as a live write."""
    body = case_body(source, "BLE_GAP_EVENT_SUBSCRIBE")
    assert "route_subscribe(" in body and "early_cccd.hold(" in body, body
    assert "apply_subscribe(" in body, body
    body = case_body(source, "BLE_GAP_EVENT_CONNECT")
    assert "early_cccd.replay(" in body, "CONNECT must apply held subscriptions"
    fill = body.index("std::fill(std::begin(subscribed)")
    opened = body.index("responses.open(")
    replay = body.index("early_cccd.replay(")
    assert fill < opened < replay and "apply_subscribe(" in body[replay:], body
    assert body.count("early_cccd.clear()") == 2, "failed and refused CONNECT clear it"
    assert "early_cccd.clear()" in case_body(source, "BLE_GAP_EVENT_DISCONNECT")
    body = gate_body(source, "apply_subscribe")
    assert "response_subscribe(" in body and "subscribed[which] = notify" in body, body


def check_link_sequence_wiring(source: str) -> None:
    """CoreS3 Android bench. The local PHY request must not cross the
    central's own procedures. MTU, CONN_UPDATE and PHY_UPDATE_COMPLETE restart
    its wait, and link_on_host reads the PHY before it asks for 2M."""
    for event in ("BLE_GAP_EVENT_MTU", "BLE_GAP_EVENT_CONN_UPDATE",
                  "BLE_GAP_EVENT_PHY_UPDATE_COMPLETE"):
        assert "defer_link_step(" in case_body(source, event), \
            f"{event} must restart the PHY wait"
    body = gate_body(source, "defer_link_step")
    assert "central_procedure_rearms(link_step)" in body and "kLinkSettleMs" in body, body
    body = gate_body(source, "link_on_host")
    assert "phy_request_needed(" in body, "link_on_host must skip a link already on 2M"
    assert body.index("ble_gap_read_le_phy(") < body.index("ble_gap_set_prefered_le_phy("), body


HCI_SENDERS = ("ble_gap_set_data_len(", "ble_gap_set_prefered_le_phy(",
               "ble_gap_update_params(", "ble_gap_read_le_phy(",
               "request_conn_params(", "request_link_parameters(")


def called_bodies(source: str, body: str, seen: set[str]):
    """(name, body) of every ble_sync.cpp function `body` reaches by direct
    calls, transitively. Calls into NimBLE or the standard library have no
    definition here and are skipped."""
    for name in sorted(set(re.findall(r"\b([A-Za-z_]\w*)\(", body))):
        if name in seen:
            continue
        seen.add(name)
        if not definition(source, name):
            continue
        callee = gate_body(source, name)
        yield name, callee
        yield from called_bodies(source, callee, seen)


def check_hci_timeout_wiring(source: str) -> None:
    """Issue #51 core dumps. Data length sent from CONNECT timed out while a
    bonded phone started encryption, and the host reset crashed. CONNECT,
    ENC_CHANGE and SUBSCRIBE, and every helper they call, send no link
    command. Data length waits for the Dle step before the PHY request, and
    every link command logs its failure."""
    for event in ("BLE_GAP_EVENT_CONNECT", "BLE_GAP_EVENT_ENC_CHANGE",
                  "BLE_GAP_EVENT_SUBSCRIBE"):
        body = case_body(source, event)
        reached = [(event, body), *called_bodies(source, body, set())]
        names = {name for name, _ in reached}
        assert {"confirm_secure", "start_link_sequence"} <= names or \
            event == "BLE_GAP_EVENT_ENC_CHANGE", (event, names)
        for name, callee in reached:
            for sender in HCI_SENDERS:
                assert sender not in callee, f"{event} reaches {sender[:-1]} through {name}"
    body = case_body(source, "BLE_GAP_EVENT_CONNECT")
    assert "link_step = LinkStep::Idle" in body and "dle_tx_octets = 0" in body, body
    assert body.index("link_step = LinkStep::Idle") < body.index("confirm_secure("), body
    body = gate_body(source, "start_link_sequence")
    assert "link_step = LinkStep::Dle" in body and "ble_npl_callout_reset(" in body, body
    body = case_body(source, "BLE_GAP_EVENT_DATA_LEN_CHG")
    assert "dle_tx_octets = event->data_len_chg.max_tx_octets" in body, body
    assert "dle_tx_octets = 0" in case_body(source, "BLE_GAP_EVENT_DISCONNECT")
    body = gate_body(source, "on_reset")
    assert "early_cccd.clear()" in body and "dle_tx_octets = 0" in body, body
    body = gate_body(source, "link_on_host")
    dle = body.index("ble_gap_set_data_len(")
    skip = body.find("dle_request_needed(dle_tx_octets")
    assert 0 <= skip < dle, "the Dle step must skip a length the central set"
    assert dle < body.index("ble_gap_read_le_phy("), "data length must precede the PHY step"
    for command, opcode in (("dle", "BLE_HCI_OCF_LE_SET_DATA_LEN"),
                            ("read_phy", "BLE_HCI_OCF_LE_RD_PHY"),
                            ("phy", "BLE_HCI_OCF_LE_SET_PHY")):
        call = rf'log_hci_fail\("{command}",\s*BLE_HCI_OP\(BLE_HCI_OGF_LE, {opcode}\)'
        assert re.search(call, body), f"link_on_host must log a failed {command}"
    body = gate_body(source, "request_conn_params")
    assert re.search(r'if \(conn_fail_log\.on_result\(rc\)\)\s*log_hci_fail\("conn",', body), \
        "request_conn_params must log a failed conn once per run"
    assert "conn_fail_log.reset()" in case_body(source, "BLE_GAP_EVENT_CONNECT")
    assert "conn_fail_log.reset()" in gate_body(source, "on_reset")
    body = gate_body(source, "log_hci_fail")
    assert "oslog.printf(" in body and "record_only" not in body, body
    assert "BLE_HS_ETIMEOUT_HCI" in body, body


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    connectivity = root / "firmware/common/connectivity/src"
    transport = (connectivity / "ble_sync.cpp").read_text()
    check_gate_wiring(transport)
    check_early_subscribe_wiring(transport)
    check_link_sequence_wiring(transport)
    check_hci_timeout_wiring(transport)
    with tempfile.TemporaryDirectory(prefix="aq-ble-policy-test-") as directory:
        executable = Path(directory) / "ble-policy-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(connectivity),
                   "-I", str(root / "firmware/common/runtime/src"),
                   str(root / "tools/test_ble_policy.cpp"),
                   str(connectivity / "ble_policy.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("BLE policy: credits Ok/Full/Closed, refused/failed, stale generation, "
          "15 s stall clock, cancel, response ring order and wrap, reopen epoch "
          "and resubscribe, EALREADY stall close, boot flag baseline, advert "
          "window, interval mode, link limits, stale auth flag served, insecure link "
          "ERROR 14 fallback, no auth code 7, subscribe restored before or with "
          "CONNECT, logged gate refusals, PHY request after central procedures "
          "and skipped on 2M, data length after the latch and skipped when set, "
          "conn HCI fail once per run, "
          "gate table and gate, subscribe, link sequence and HCI timeout "
          "wiring passed")


if __name__ == "__main__":
    main()
