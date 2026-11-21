"""Host tests for stack_budget.py on synthetic GCC call graphs and a tiny ELF; no SDK build is read."""

from __future__ import annotations

import contextlib
import faulthandler
import io
import json
from pathlib import Path
import stat
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import stack_budget as sb


def node(title: str, name: str, frame: int, qualifier: str = "static", where: str = "src/a.c:1:1") -> str:
    return f'node: {{ title: "{title}" label: "{name}\\n{where}\\n{frame} bytes ({qualifier})" }}'


def edge(source: str, target: str) -> str:
    return f'edge: {{ sourcename: "{source}" targetname: "{target}" label: "" }}'


def write_ci(path: Path, lines: list[str]) -> Path:
    path.write_text("graph: { title: \"x\"\n" + "\n".join(lines) + "\n}\n")
    return path


def tiny_elf(path: Path, sections: dict[str, int]) -> None:
    """A 32-bit little-endian ELF with only section headers, enough for elf_sections()."""
    names = b"\0" + b"".join(name.encode() + b"\0" for name in [*sections, ".shstrtab"])
    header_offset = 52 + len(names)
    headers = [struct.pack("<10I", *([0] * 10))]
    cursor = 1
    for name, size in sections.items():
        headers.append(struct.pack("<10I", cursor, 1, 2 | 4, 0, 0, size, 0, 0, 4, 0))
        cursor += len(name) + 1
    headers.append(struct.pack("<10I", cursor, 3, 0, 0, 52, len(names), 0, 0, 1, 0))
    ident = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    elf = ident + struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, 0, 0, header_offset, 0, 52, 0, 0, 40,
                              len(headers), len(headers) - 1)
    path.write_bytes(elf + names + b"".join(headers))


def check_graph(root: Path) -> None:
    ci = write_ci(root / "a.ci", [
        node("worker", "void worker(void*)", 200),
        node("/src/a.c:helper", "int helper(int)", 64),
        node("walk", "void walk(int)", 96),
        node("vla", "void vla(int)", 40, "dynamic"),
        node("bounded", "void bounded(int)", 24, "dynamic,bounded"),
        node("/src/a.c:_ZN3Log6printfEPKcz", ")", 352),
        node("dispatch", "void dispatch()", 16),
        node("hook_a", "void hook_a()", 500, where="firmware/common/x.cpp:1:1"),
        node("hook_b", "void hook_b()", 900, where="firmware/board/y.cpp:1:1"),
        node("relay", "void relay()", 32),
        edge("worker", "/src/a.c:helper"), edge("worker", "walk"), edge("worker", "dispatch"),
        edge("walk", "walk"),
        edge("/src/a.c:helper", "vla"), edge("/src/a.c:helper", "bounded"),
        edge("/src/a.c:helper", "/src/a.c:_ZN3Log6printfEPKcz"),
        edge("dispatch", "__indirect_call"), edge("hook_a", "__indirect_call"), edge("relay", "__indirect_call"),
    ])
    graph = sb.load_ci([ci])
    # A variadic method GCC prints as ")" keeps its symbol.
    assert graph.functions["/src/a.c:_ZN3Log6printfEPKcz"].name == "_ZN3Log6printfEPKcz"
    assert graph.functions["worker"].calls == {"/src/a.c:helper", "walk", "dispatch"}
    assert graph.functions["dispatch"].indirect and not graph.functions["worker"].indirect

    # Prebuilt code takes its frame from `entry` and its calls from call8.
    sb.add_disassembly(graph, "\n".join([
        "40000000 <blob>:",
        "40000000:\tentry\ta1, 48",
        "40000003:\tcall8\t40000010 <helper>",
        "40000006:\tcall8\t40000020 <pool+0x4>",
        "40000009:\tcallx8\ta8",
        "4000000c:\tcall0\t40000030 <spill>",
        "4000000f:\tcall0\t40000040 <windowed>",
        "40000012:\tcall0\t40000010 <helper>",
        "40000010 <helper>:",
        "40000010:\tentry\ta1, 999",
        "40000030 <spill>:",
        "40000030:\tret",
        "40000040 <windowed>:",
        "40000040:\tentry\ta1, 32",
    ]))
    # A call0 reaches prebuilt code with no `entry`. One that lands on a
    # windowed function is data decoded as an instruction and is dropped.
    assert graph.functions["blob"].frame == 48 and graph.functions["blob"].calls == {"helper", "spill"}
    assert graph.functions["blob"].indirect
    assert "helper" not in graph.functions, "a static function's bare symbol keeps its .ci frame"

    # No rule, so the pointer call is unresolved and adds nothing.
    result = sb.analyse(graph, ["worker"], [], 2)
    assert result.worst == 200 + 64 + 352, result.worst
    assert [frame for _, frame in result.path] == [200, 64, 352]
    assert result.unresolved_indirect == 1
    assert result.recursion == [["void walk(int) [src/a.c:1]"]], result.recursion
    assert result.pointer_cycles == 0
    assert any("(dynamic, 40 B)" in item for item in result.dynamic)
    assert any("(dynamic,bounded, 24 B)" in item for item in result.dynamic)

    # A scoped rule reaches only from its callers, and the file filter picks hook_a.
    rules = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["firmware/common/*::hook_*"]}])
    assert rules == [(frozenset({"dispatch"}), ["hook_a"])]
    result = sb.analyse(graph, ["worker"], rules, 2)
    assert result.worst == 200 + 16 + 500, result.worst
    assert result.unresolved_indirect == 1, "hook_a has a pointer call no rule covers"

    # The depth cap stops a chain of pointer calls.
    chain = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["hook_a"]},
                                      {"from": ["hook_a"], "to": ["relay"]},
                                      {"from": ["relay"], "to": ["hook_b"]}])
    assert sb.analyse(graph, ["worker"], chain, 3).worst == 200 + 16 + 500 + 32 + 900
    capped = sb.analyse(graph, ["worker"], chain, 2)
    assert capped.worst == 200 + 16 + 500 + 32 and capped.unresolved_indirect == 1

    # A cycle closed through a pointer call is counted, not listed as recursion.
    loop = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["hook_a"]}, {"from": ["hook_a"], "to": ["dispatch"]}])
    looped = sb.analyse(graph, ["dispatch"], loop, 2)
    assert looped.pointer_cycles == 1 and looped.recursion == []

    # A plain string rule applies to every pointer call.
    loose = sb.indirect_rules(graph, ["relay"])
    assert loose == [(None, ["relay"])]


def check_aliases_and_cycles(root: Path) -> None:
    # A frameless C1 or D1 alias keeps its edge to the body, a declaration stays unknown.
    ci = write_ci(root / "alias.ci", [
        node("caller", "void caller()", 32),
        'node: { title: "_ZN4LockD1Ev" label: "Lock::~Lock()\\nsrc/lock.cpp:9:3" shape : triangle }',
        node("_ZN4LockD2Ev", "Lock::~Lock()", 48),
        'node: { title: "xQueueGenericSend" label: "xQueueGenericSend\\nqueue.h:1:1" shape : ellipse }',
        node("send", "void send()", 64),
        edge("caller", "_ZN4LockD1Ev"), edge("_ZN4LockD1Ev", "_ZN4LockD2Ev"), edge("_ZN4LockD2Ev", "send"),
        edge("send", "xQueueGenericSend"),
    ])
    graph = sb.load_ci([ci])
    alias = graph.functions["_ZN4LockD1Ev"]
    assert alias.frame == 0 and alias.calls == {"_ZN4LockD2Ev"} and alias.source == "ci", alias
    assert "xQueueGenericSend" not in graph.functions
    assert sb.analyse(graph, ["caller"], [], 2).worst == 32 + 48 + 64

    # b is first reached inside the a and b cycle, where the cut hides a's
    # 900 B. The entry later reaches b through c without a, which counts a.
    # a's complete result already holds b's frame, so b counts twice there.
    # That errs high, never low.
    ci = write_ci(root / "cycle.ci", [
        node("entry", "void entry()", 8),
        node("a", "void a()", 900),
        node("b", "void b()", 16),
        node("c", "void c()", 1000),
        edge("entry", "a"), edge("entry", "c"), edge("a", "b"), edge("b", "a"), edge("c", "b"),
    ])
    result = sb.analyse(sb.load_ci([ci]), ["entry"], [], 2)
    assert result.worst == 8 + 1000 + 16 + 900 + 16, result.worst
    assert [frame for _, frame in result.path] == [8, 1000, 16, 900], result.path


def make_build(root: Path, sections: dict[str, int]) -> tuple[Path, dict]:
    build = root / "build"
    build.mkdir()
    write_ci(build / "main.ci", [
        node("main_task", "void main_task(void*)", 300),
        node("leaf", "void leaf()", 100),
        edge("main_task", "leaf"),
        edge("main_task", "blob"),
    ])
    tiny_elf(build / "app.elf", sections)
    (build / "project_description.json").write_text(json.dumps({"app_elf": "app.elf"}))
    (build / "sdkconfig").write_text("CONFIG_MAIN_STACK=4096\n")
    listing = build / "listing.txt"
    listing.write_text("40000000 <blob>:\n40000000:\tentry\ta1, 160\n")
    objdump = build / "fake-objdump"
    objdump.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.write(open({str(listing)!r}).read())\n")
    objdump.chmod(objdump.stat().st_mode | stat.S_IXUSR)
    (build / "CMakeCache.txt").write_text(f"CMAKE_OBJDUMP:FILEPATH={objdump}\n")
    budgets = {
        "policy": {"stack_headroom_percent": 10, "stack_round_bytes": 64, "section_headroom_percent": 2,
                   "section_round_bytes": 256, "reserve_bytes": 512},
        "shared_indirect": [],
        "tasks": {"main": {"entry": ["main_task"], "stack_sdkconfig": "CONFIG_MAIN_STACK"},
                  "tight": {"entry": ["main_task"]}},
        "images": {"fake/app": {"build": str(build),
                                "tasks": {"main": {}, "tight": {"stack_bytes": 1000}},
                                "sections": {".flash.text": 0, ".dram0.bss": 0}}},
    }
    return build, budgets


def site(name: str, stack: int | None, entry: str = "main_task", path: str = "src/a.c") -> sb.TaskSite:
    return sb.TaskSite(f"{path}:7", path, entry, name, stack, str(stack))


def run(argv: list[str], sites: list[sb.TaskSite] | None = None) -> tuple[int, str, str]:
    """main() with the repo source scan replaced by task sites in the fake build."""
    scan = sb.SourceScan(sites=[site("tight", 1000)] if sites is None else sites)
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
            patch.object(sb, "scan_sources", return_value=scan):
        code = sb.main(argv)
    return code, out.getvalue(), err.getvalue()


def check_cli(root: Path) -> None:
    build, budgets = make_build(root, {".flash.text": 10_000, ".dram0.bss": 300})
    path = root / "budgets.json"
    path.write_text(json.dumps(budgets))

    # Zero section budgets fail the check, and report mode only warns.
    code, out, err = run(["--budgets", str(path), "--check"])
    assert code == 1 and ".flash.text: 10000 B is past its budget 0 B" in err, err
    assert run(["--budgets", str(path)])[0] == 0
    assert "| main | 4,096 | 460 | 972 | 3,124 |" in out, out

    # --write-budgets records headroom, capped at the stack minus the reserve.
    assert run(["--budgets", str(path), "--write-budgets"])[0] == 0
    written = json.loads(path.read_text())["images"]["fake/app"]
    assert written["tasks"]["main"] == {"measured": 460, "budget": 512}
    assert written["tasks"]["tight"] == {"stack_bytes": 1000, "measured": 460, "budget": 488}
    assert written["sections"] == {".flash.text": 10_240, ".dram0.bss": 512}
    assert written["sections_measured"] == {".flash.text": 10_000, ".dram0.bss": 300}
    code, out, err = run(["--budgets", str(path), "--check", "--path"])
    assert code == 0, err
    assert "       300  void main_task(void*)" in out and "       160  blob" in out, out
    assert "Task source check passed" in out and "Internal stack check skipped" in out, out
    clean = path.read_text()

    # Mutation: a lowered task budget fails, and so does a stack the worst case outgrows.
    mutated = json.loads(path.read_text())
    mutated["images"]["fake/app"]["tasks"]["main"]["budget"] = 400
    mutated["images"]["fake/app"]["tasks"]["tight"]["stack_bytes"] = 900
    path.write_text(json.dumps(mutated))
    code, _, err = run(["--budgets", str(path), "--check"])
    assert code == 1
    assert "main: static worst 460 B is past its budget 400 B" in err, err
    assert "tight: static worst 460 B plus the 512 B reserve is past the 900 B stack" in err, err
    assert "tight stack_bytes 900 does not match the 1000 B stack at src/a.c:7" in err, err

    # Mutation: a deleted budget fails instead of passing with a flag.
    mutated = json.loads(clean)
    del mutated["images"]["fake/app"]["tasks"]["main"]["budget"]
    path.write_text(json.dumps(mutated))
    code, out, err = run(["--budgets", str(path), "--check"])
    assert code == 1 and "fake/app main: no budget" in err and "NO BUDGET" in out, err

    # Mutation: a new xTaskCreate the budget file does not track fails, and so
    # does a stack the source scan cannot read.
    path.write_text(clean)
    code, _, err = run(["--budgets", str(path), "--check"], [site("tight", 1000), site("aq-new-task", 2048)])
    assert code == 1 and 'creates task "aq-new-task", which no task in this image tracks' in err, err
    code, _, err = run(["--budgets", str(path), "--check"], [site("tight", None)])
    assert code == 1 and "with a stack the scan cannot read" in err, err
    # A site in a file this image does not link belongs to another image, so
    # the tracked stack_bytes here is stale.
    code, _, err = run(["--budgets", str(path), "--check"], [site("tight", 1000, path="src/other.c")])
    assert code == 1 and 'tight has stack_bytes but this image creates no task "tight"' in err, err
    # The file links, so its sites count even when the entry is defined in
    # another file the graph never saw.
    code, _, err = run(["--budgets", str(path), "--check"],
                       [site("tight", 1000), site("aq-new-task", 2048, entry="defined_elsewhere")])
    assert code == 1 and 'creates task "aq-new-task", which no task in this image tracks' in err, err
    # A call the scan cannot parse (a lambda entry or a name in a variable)
    # fails closed in an image that links its file.
    unparsed = sb.TaskSite("src/a.c:9", "src/a.c", "", "", None, "xTaskCreate(task_name_var", parsed=False)
    code, _, err = run(["--budgets", str(path), "--check"], [site("tight", 1000), unparsed])
    assert code == 1 and "src/a.c:9 creates a task the scan cannot parse (xTaskCreate(task_name_var" in err, err
    # A second creation site for one tracked name fails. Each copy takes its
    # own stack from the heap, which the check does not count.
    code, _, err = run(["--budgets", str(path), "--check"],
                       [site("tight", 1000), sb.TaskSite("src/a.c:12", "src/a.c", "main_task", "tight", 1000, "1000")])
    assert code == 1 and 'task "tight" is created at 2 sites (src/a.c:7, src/a.c:12)' in err, err
    # Two tracked keys that share one task name report the duplicate once.
    shared = json.loads(clean)
    shared["tasks"]["also"] = {"entry": ["main_task"], "task_name": "tight"}
    shared["images"]["fake/app"]["tasks"]["also"] = {"stack_bytes": 1000}
    path.write_text(json.dumps(shared))
    code, _, err = run(["--budgets", str(path), "--check"],
                       [site("tight", 1000), sb.TaskSite("src/a.c:12", "src/a.c", "main_task", "tight", 1000, "1000")])
    assert code == 1 and err.count("is created at 2 sites") == 1, err
    path.write_text(clean)
    # Sites in different branches of one #if group are alternatives, so one
    # name in each is a single site. Every branch is still checked against
    # the budget, and two sites in the same branch are still two.
    first, second = ("src/a.c:3", 0), ("src/a.c:3", 1)
    branch_sites = [sb.TaskSite("src/a.c:4", "src/a.c", "main_task", "tight", 1000, "1000", branches=(first,)),
                    sb.TaskSite("src/a.c:6", "src/a.c", "main_task", "tight", 1000, "1000", branches=(second,))]
    code, _, err = run(["--budgets", str(path), "--check"], branch_sites)
    assert code == 0, err
    code, _, err = run(["--budgets", str(path), "--check"], [*branch_sites, site("tight", 1000)])
    assert code == 1 and 'task "tight" is created at 3 sites' in err, err
    code, _, err = run(["--budgets", str(path), "--check"],
                       [branch_sites[0], sb.TaskSite("src/a.c:6", "src/a.c", "main_task", "tight", 900, "900",
                                                     branches=(second,))])
    assert code == 1 and "tight stack_bytes 1000 does not match the 900 B stack at src/a.c:6" in err, err
    code, _, err = run(["--budgets", str(path), "--check"],
                       [branch_sites[0], sb.TaskSite("src/a.c:6", "src/a.c", "main_task", "aq-other", 1000, "1000",
                                                     branches=(second,))])
    assert code == 1 and 'creates task "aq-other", which no task in this image tracks' in err, err
    # --write-budgets refuses while the stack sizes disagree.
    assert run(["--budgets", str(path), "--write-budgets"], [site("tight", 2000)])[0] == 2
    assert path.read_text() == clean
    assert run(["--budgets", str(path), "--check"])[0] == 0

    # A missing build or an unknown image is a usage error, not a pass.
    assert run(["--budgets", str(path), "--image", "nope/none"])[0] == 2
    (build / "project_description.json").unlink()
    assert run(["--budgets", str(path), "--image", "fake/app"])[0] == 2


def check_scrub(root: Path) -> None:
    """A scrub frame must cover each named subtree plus its margin (#172)."""
    build, budgets = make_build(root, {".flash.text": 10_000, ".dram0.bss": 300})
    write_ci(build / "gnss.ci", [
        node("scrub", "void ns::scrub_stack()", 600),
        node("on_line", "ns::poll(int)::<lambda(int)>", 100),
        node("parse", "int ns::Dialect::parse(int)", 300),
        node("convert", "int ns::convert(int)", 150),
        node("deep", "int ns::deep(int)", 90),
        edge("on_line", "parse"), edge("on_line", "convert"), edge("convert", "deep"),
    ])
    path = root / "budgets.json"
    path.write_text(json.dumps(budgets))
    assert run(["--budgets", str(path), "--write-budgets"])[0] == 0
    budgets = json.loads(path.read_text())
    budgets["tasks"]["main"]["scrub"] = {"function": "*ns::scrub_stack()", "margin_bytes": 128,
                                         "covers": ["*ns::poll(int)::<lambda*", "*ns::convert(*"]}
    path.write_text(json.dumps(budgets))
    code, out, err = run(["--budgets", str(path), "--check"])
    assert code == 0, err
    assert "Scrub check passed. main scrub frame 600 B covers 400 B with 128 B margin." in out, out

    graph = sb.load_ci([build / "gnss.ci"])
    spec = budgets["tasks"]["main"]["scrub"]
    line, findings = sb.scrub_check(graph, "main", spec, [], 2)
    assert line and not findings, findings
    # Mutation: a parse that grows past the frame less the margin fails.
    line, findings = sb.scrub_check(graph, "main", {**spec, "margin_bytes": 256}, [], 2)
    assert findings == ["main: *ns::poll(int)::<lambda* uses 400 B below its caller, past the 600 B "
                        "scrub frame less its 256 B margin"], findings
    # Mutation: a renamed or inlined cover, and a scrub pattern that is not unique.
    _, findings = sb.scrub_check(graph, "main", {**spec, "covers": ["*ns::gone(*"]}, [], 2)
    assert findings and "the scrub cover *ns::gone(* matches nothing" in findings[0], findings
    _, findings = sb.scrub_check(graph, "main", {**spec, "function": "*ns::*"}, [], 2)
    assert findings and "matches 5 functions, it must match one" in findings[0], findings
    # A pointer call reaches the parse only through the task's indirect rules.
    graph.functions["on_line"].calls.discard("parse")
    graph.functions["on_line"].indirect = True
    _, findings = sb.scrub_check(graph, "main", spec, [(None, ["parse"])], 2)
    assert not findings, findings
    assert sb.analyse(graph, ["on_line"], [(None, ["parse"])], 2).worst == 400

    # The CLI fails --check when the scrub stops covering.
    budgets["tasks"]["main"]["scrub"]["margin_bytes"] = 256
    path.write_text(json.dumps(budgets))
    code, out, err = run(["--budgets", str(path), "--check"])
    assert code == 1 and "fake/app scrub: main: *ns::poll(int)::<lambda* uses 400 B" in err, err
    assert "Scrub check failed." in out, out


def check_internal_stacks(root: Path) -> None:
    source = root / "firmware" / "common" / "x.cpp"
    source.parent.mkdir(parents=True)
    source.write_text("void a() { xTaskCreate(f, \"ok\", 4096, nullptr, 1, nullptr); }\n")
    skipped = root / "firmware" / "board" / "managed_components" / "lib" / "y.c"
    skipped.parent.mkdir(parents=True)
    skipped.write_text("xTaskCreateWithCaps(f, \"lib\", 4096, 0, 1, 0, MALLOC_CAP_SPIRAM);\n")
    assert sb.external_stack_sources(root) == []
    source.write_text("void a() {\n  xTaskCreateStatic (f, \"psram\", 4096, nullptr, 1, stack, &tcb);\n}\n")
    assert sb.external_stack_sources(root) == ["firmware/common/x.cpp:2"]
    # Comments do not count, and a string holding // does not hide the rest of its line.
    source.write_text("// xTaskCreateStatic(f, ...)\n/* xTaskCreateWithCaps(\n */ const char *u = \"http://x\";"
                      " xTaskCreateStatic(f, \"s\", 1, 0, 1, 0, 0);\n")
    assert sb.external_stack_sources(root) == ["firmware/common/x.cpp:3"]

    # The scan reads names and stacks, a constant expression or a member default.
    source.write_text("struct O { std::uint32_t task_stack_bytes = 8192; };\n"
                      "void a(O o) {\n  xTaskCreate(lan_task, \"os-lan\",\n    o.task_stack_bytes, nullptr, 1, nullptr);\n"
                      "  xTaskCreate(board_task, \"board\", 16 * 1024, nullptr, 1, nullptr);\n"
                      "  // xTaskCreate(old_task, \"old\", 1024, nullptr, 1, nullptr);\n"
                      "  xTaskCreate(w, \"w\", static_cast<std::uint32_t>(c.worker_stack_bytes), nullptr, 1, 0);\n}\n"
                      "struct C { std::size_t worker_stack_bytes = 2048; };\nvoid b(C c) { c.worker_stack_bytes = 4096; }\n")
    sites = {item.name: item for item in sb.scan_sources(root).sites}
    assert set(sites) == {"os-lan", "board", "w"}, sites
    assert sites["os-lan"].stack == 8192 and sites["os-lan"].where == "firmware/common/x.cpp:3"
    assert sites["os-lan"].entry == "lan_task"
    assert sites["board"].stack == 16384
    assert sites["w"].stack is None, "two values for one member are ambiguous"
    assert all(item.parsed for item in sites.values())
    # A lambda entry, a name in a variable and any other xTaskCreate* form are
    # kept unparsed, so the image check fails closed on them.
    source.write_text("void a(const char *n) {\n"
                      "  xTaskCreate([](void *) {}, \"lambda\", 2048, nullptr, 1, nullptr);\n"
                      "  xTaskCreatePinnedToCore(task, n, 2048, nullptr, 1, nullptr, 0);\n"
                      "  xTaskCreate(defined_elsewhere, \"ok\", 2048, nullptr, 1, nullptr);\n}\n")
    sites = sb.scan_sources(root).sites
    assert [(item.where, item.parsed) for item in sites] == [
        ("firmware/common/x.cpp:2", False), ("firmware/common/x.cpp:3", False),
        ("firmware/common/x.cpp:4", True)], sites
    assert sites[0].stack_text.startswith("xTaskCreate([](void *)") and sites[2].entry == "defined_elsewhere"
    # A string literal that mentions a task call is text, not a call.
    source.write_text("void a() {\n  ESP_LOGI(\"t\", \"xTaskCreate(failed)\");\n"
                      "  puts(\"xTaskCreateStatic(x) \\\" xTaskCreate(\");\n"
                      "  xTaskCreate(board_task, \"board\", 4096, nullptr, 1, nullptr);\n}\n")
    scan = sb.scan_sources(root)
    assert scan.external == [], scan.external
    assert [(item.where, item.name, item.stack, item.parsed) for item in scan.sites] == [
        ("firmware/common/x.cpp:4", "board", 4096, True)], scan.sites
    # A digit separator is not a character literal, so its quote cannot pair
    # with a later one and hide the call between them.
    source.write_text("int n = 4'096; xTaskCreate(t, \"ds\", 4096, nullptr, 1, nullptr); char c = 'x';\n"
                      "auto w = L'\"'; xTaskCreate(u, \"wide\", 2048, nullptr, 1, nullptr);\n")
    assert [(item.name, item.stack) for item in sb.scan_sources(root).sites] == [
        ("ds", 4096), ("wide", 2048)], sb.scan_sources(root).sites
    # A raw string ends only at its own delimiter, so a quote inside it does
    # not open a literal that hides the call after it. Its text is not a call.
    source.write_text('auto r = R"(he said ")"; xTaskCreate(t, "raw", 4096, nullptr, 1, nullptr); auto q = "z";\n'
                      'auto d = u8R"x(xTaskCreate(f)" )x"; xTaskCreate(v, "delim", 1024, nullptr, 1, nullptr);\n'
                      'auto m = R"(\nxTaskCreateStatic(\n)";\n')
    scan = sb.scan_sources(root)
    assert [(item.where, item.name) for item in scan.sites] == [
        ("firmware/common/x.cpp:1", "raw"), ("firmware/common/x.cpp:2", "delim")], scan.sites
    assert scan.external == [], scan.external
    # Comment stripping uses the same literal rules, so a // inside a raw
    # string does not hide the rest of its line, and one after 4'096 counts.
    assert sb.strip_comments('R"(//)" x; 4\'0 // c') == 'R"(//)" x; 4\'0     '
    # A character literal right after a keyword is still a literal, so its
    # quote cannot open a string that hides the call after it.
    source.write_text("switch (c) { case'\"': xTaskCreate(t, \"kw\", 4096, nullptr, 1, nullptr); }"
                      " auto q = \"x\";\nreturn'\"'; xTaskCreate(u, \"ret\", 2048, nullptr, 1, nullptr); \"y\";\n"
                      # The digit of an encoding prefix does not start a number with a separator.
                      "auto e = u8'a'; xTaskCreate(v, \"u8\", 1024, nullptr, 1, nullptr); char d = 'z';\n")
    assert [(item.where, item.name) for item in sb.scan_sources(root).sites] == [
        ("firmware/common/x.cpp:1", "kw"), ("firmware/common/x.cpp:2", "ret"),
        ("firmware/common/x.cpp:3", "u8")], sb.scan_sources(root).sites
    assert sb.blank_literals("case'\"': 0x1'0'0 L'\"' u8\"a\"") == "case' ': 0x1'0'0 L' ' u8\" \""
    # A string right after a macro name, as in "%"PRIu32"\n", is still a string.
    assert sb.blank_literals('f("a"PRIu32"xTaskCreate(");') == 'f(" "PRIu32"            ");'

    # Each #if, #elif and #else starts a new branch of its group, groups nest,
    # and every branch is scanned. A directive inside a raw string, or on a
    # line continued from a directive, is not one.
    source.write_text("#if CONFIG_A\n"                                                # 1
                      "xTaskCreate(a, \"dup\", 4096, nullptr, 1, nullptr);\n"       # 2
                      "#elif CONFIG_B\n"                                               # 3
                      "xTaskCreate(b, \"dup\", 4096, nullptr, 1, nullptr);\n"       # 4
                      "  #  else\n"                                                    # 5
                      "auto r = R\"(\n#endif\n)\";\n"                              # 6-8
                      "xTaskCreate(c, \"dup\", 4096, nullptr, 1, nullptr);\n"       # 9
                      "#endif\n"                                                       # 10
                      "#ifdef X \\\n#else\n"                                        # 11-12
                      "#ifndef Y\n"                                                    # 13
                      "xTaskCreate(d, \"nest\", 4096, nullptr, 1, nullptr);\n"      # 14
                      "#endif\n"                                                       # 15
                      "xTaskCreate(e, \"nest\", 4096, nullptr, 1, nullptr);\n"      # 16
                      "#endif\n"                                                       # 17
                      "xTaskCreate(f, \"top\", 4096, nullptr, 1, nullptr);\n")      # 18
    sites = sb.scan_sources(root).sites
    name = "firmware/common/x.cpp"
    assert [(item.entry, item.branches) for item in sites] == [
        ("a", ((f"{name}:1", 0),)), ("b", ((f"{name}:1", 1),)), ("c", ((f"{name}:1", 2),)),
        ("d", ((f"{name}:11", 0), (f"{name}:13", 0))), ("e", ((f"{name}:11", 0),)), ("f", ())], sites
    assert sb.exclusive(sites[0], sites[1]) and sb.exclusive(sites[1], sites[2]) and sb.exclusive(sites[2], sites[0])
    assert not sb.exclusive(sites[3], sites[4]) and not sb.exclusive(sites[4], sites[3])
    assert not sb.exclusive(sites[0], sites[5]) and not sb.exclusive(sites[5], sites[0])
    # A line comment ending in a backslash goes on over the next line, so a
    # #else there is comment text and both calls share one branch.
    source.write_text("#if A\nxTaskCreate(a, \"c\", 4096, nullptr, 1, nullptr);\n// note \\\n#else\n"
                      "xTaskCreate(b, \"c\", 4096, nullptr, 1, nullptr);\n#endif\n")
    sites = sb.scan_sources(root).sites
    assert [item.branches for item in sites] == [((f"{name}:1", 0),)] * 2, sites
    assert not sb.exclusive(sites[0], sites[1])

    assert sb.constant("(4 + 4) * 1024") == 8192 and sb.constant("1 << 4") is None
    allowed = {sb.EXTERNAL_STACK_CONFIG: "y"}
    findings = sb.stack_memory_findings(allowed, ["firmware/common/x.cpp:2"])
    assert findings and "firmware/common/x.cpp:2" in findings[0]
    assert sb.stack_memory_findings({}, ["firmware/common/x.cpp:2"]) == []
    # The committed firmware only uses xTaskCreate, and the scan reads every stack.
    scan = sb.scan_sources(sb.ROOT)
    assert scan.external == []
    assert scan.sites and all(item.parsed and item.stack for item in scan.sites), scan.sites


def check_runtime_report() -> None:
    """`parquet mem` is answered on the board task and parquet_device waits for the heap line."""
    source = (sb.ROOT / "firmware" / "common" / "logger" / "src" / "os_logger.cpp").read_text()
    code = " ".join(source.split())
    assert 'std::strcmp(input.text, "parquet mem") == 0) report_runtime(input.received_mono_us, true);' in code
    # New fields go at the end so a reader of the older lines keeps working.
    assert '"nimble_host=%ld tcpip=%ld uptime_s=%lld esp_timer=%ld sys_evt=%ld\\n"' in code
    assert '"psram_min=%lu psram_largest=%lu\\n"' in code

    import parquet_device
    tasks = "DIAG tasks os-board=5000 parquet-sd=12000 os-lan=-1 os_ble_error=900"
    heap = "DIAG heap internal_free=1 internal_min=1 internal_largest=1 psram_largest=1"
    printed: list[str] = []
    with patch.object(parquet_device, "send_command") as send, \
            patch.object(parquet_device, "lines_until", return_value=iter([tasks, heap, "PARQUET ROW"])), \
            patch("builtins.print", side_effect=lambda line, **_: printed.append(line)):
        assert parquet_device.command(None, SimpleNamespace(text="parquet  mem", timeout=1)) == 0
    send.assert_called_once_with(None, "parquet mem")
    assert printed == [tasks, heap]


def check_logger_hooks_traced(root: Path) -> None:
    """Every board logger main.cpp hands lambdas to oslogger::Hooks. The board
    and parquet-sd rules must trace them like the bringup and component ones
    (issue #198), for each logger found in the tree, so a new board is covered."""
    budgets = json.loads(sb.DEFAULT_BUDGETS.read_text())
    mains = sorted((sb.ROOT / "firmware").glob("esp-idf-*/logger/main.cpp"))
    assert len(mains) >= 3, "no board logger main.cpp found"
    lines = []
    for main in mains:
        where = f"{main.relative_to(sb.ROOT)}:1:1"
        lines.append(node(f"hook_{main.parent.parent.name}",
                          "{anonymous}::kHooks::{lambda(void*)#1}::_FUN(void*)", 16, where=where))
    graph = sb.load_ci([write_ci(root / "hooks.ci", lines)])
    for task_name in ("parquet-sd", "board"):
        rules = [rule for rule in budgets["tasks"][task_name]["indirect"]
                 if any(target.startswith("firmware/esp-idf-") for target in rule["to"])]
        assert rules, f"{task_name} has no board hook rule"
        for rule in rules:
            matched = {title for pattern in rule["to"] for title in sb.pattern_matches(graph, pattern)}
            for title in graph.functions:
                assert title in matched, f"{task_name} does not trace the hook {title}"


def check_committed_budgets() -> None:
    budgets = json.loads(sb.DEFAULT_BUDGETS.read_text())
    policy = budgets["policy"]
    for name, spec in budgets["images"].items():
        for task_name, task in spec["tasks"].items():
            assert task_name in budgets["tasks"], f"{name} names an undefined task {task_name}"
            assert "budget" in task and "measured" in task, f"{name} {task_name} has no budget"
            assert task["budget"] >= task["measured"], f"{name} {task_name} budget below measured"
            if "stack_bytes" in task and "budget" in task:
                assert task["budget"] + policy["reserve_bytes"] <= task["stack_bytes"], f"{name} {task_name}"
        for section, budget in spec["sections"].items():
            assert budget >= spec["sections_measured"][section], f"{name} {section}"


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        (root / "graph").mkdir()
        (root / "cli").mkdir()
        (root / "src").mkdir()
        check_graph(root / "graph")
        (root / "alias").mkdir()
        check_aliases_and_cycles(root / "alias")
        check_cli(root / "cli")
        (root / "scrub").mkdir()
        check_scrub(root / "scrub")
        check_internal_stacks(root / "src")
    check_runtime_report()
    check_committed_budgets()
    with tempfile.TemporaryDirectory() as raw:
        check_logger_hooks_traced(Path(raw))
    print("stack_budget tests passed")


if __name__ == "__main__":
    faulthandler.dump_traceback_later(120, exit=True)
    main()
