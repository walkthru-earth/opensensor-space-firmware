"""Static stack worst case per task and linker section sizes per image (issue #37).

Board builds compile every object with `-fstack-usage -fcallgraph-info=su`
(firmware/cmake/os_stack_usage.cmake), so GCC leaves a `.ci` call graph with
each function's frame size next to every object. This tool joins those graphs,
walks them from each tracked task's entry and prints the deepest path. Functions
with no `.ci` (prebuilt newlib, libgcc and the Wi-Fi and Bluetooth blobs) take
their frame from the `entry a1, N` instruction and their calls from the
disassembly of the linked ELF. ROM functions have neither and count as zero.

Calls through a function pointer resolve to the `indirect` rules in
tools/stack_budget.json. A rule names the callers it applies to and the
functions they may reach, so a pointer call reaches only reviewed targets. One
path takes at most `indirect_depth` pointer calls (2 unless the task says
otherwise, an event loop calling a library handler that calls our hook). A
pointer call past that, or one no rule covers, is counted as unresolved and
adds nothing.
Recursion is cut where it closes and flagged, and so are frames GCC marks
dynamic (alloca or a variable length array). Both mean the figure is not a
bound, so a flagged function in our code needs a reviewed depth cap or an
iterative rewrite.

The worst case excludes the interrupt context frame and register window
spills that FreeRTOS keeps on a task stack, see `reserve_bytes` in the budget
file. It is a static estimate, a bench high water mark (`parquet mem`) is the
measured counterpart.

`--check` also fails on a tracked task with no budget, and it scans repo
firmware (comments and string contents skipped) for every `xTaskCreate*` call in a file the image
links (any of its functions in the call graph). Each one must name a tracked
task (`task_name`, the task key by default) whose `stack_bytes` equals the
stack size in source, so a new task or a resized stack cannot pass unnoticed.
A call it cannot parse, such as a lambda entry or a name in a variable, fails,
and so does a second creation site for one tracked task name.

The scan does not evaluate the preprocessor. Every branch of every `#if`,
`#elif` and `#else` is scanned, `#if 0` included, so a call in any branch must
name a tracked task whose `stack_bytes` matches its stack. Two calls with one
task name pass the one site rule only when they sit in different branches of
the same `#if` group, since at most one of them is compiled. Calls in sibling
files, or in the same branch, still count as two sites.

A task may name a `scrub`, a function that zeroes its own frame to wipe
dead frames under its caller (the GNSS reader after each position parse,
#172). `--check` fails when that frame is smaller than the deepest subtree of
any `covers` pattern plus `margin_bytes`, or when a pattern matches nothing,
so a parse that grows past the scrub cannot pass unnoticed.

Usage:
  pixi run stack-budget                         # every image with a build
  pixi run stack-budget --image cores3/bringup --path
  pixi run stack-budget --check --image waveshare/logger   # CI, fails past budget
  pixi run stack-budget --write-budgets         # after a reviewed change
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import json
import math
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUDGETS = ROOT / "tools" / "stack_budget.json"
INDIRECT = "__indirect_call"

NODE_RE = re.compile(r'^node: \{ title: "([^"]*)" label: "([^"]*)"')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"')
FRAME_RE = re.compile(r"(\d+) bytes \(([a-z,]+)\)")
FUNC_RE = re.compile(r"^([0-9a-f]{8}) <([^>]+)>:$")
ENTRY_RE = re.compile(r"\tentry\ta1, (\d+)")
# A real call targets a function start. A target with an offset is a literal
# pool word decoded as an instruction, so it is ignored.
CALL_RE = re.compile(r"\tcall(?:0|4|8|12)\t[0-9a-f]+ <([^>+]+)>$")
CALLX_RE = re.compile(r"\tcallx(?:0|4|8|12)\t")


class BudgetError(RuntimeError):
    pass


@dataclass
class Function:
    name: str  # demangled signature, or the symbol for disassembled code
    location: str = ""
    frame: int = 0
    qualifier: str = "static"  # static, dynamic or dynamic,bounded
    source: str = "ci"  # ci, disassembly or unknown
    calls: set[str] = field(default_factory=set)
    indirect: bool = False


@dataclass
class Graph:
    functions: dict[str, Function] = field(default_factory=dict)

    def resolve(self, title: str) -> str:
        return title if title in self.functions else self._unknown(title)

    def _unknown(self, title: str) -> str:
        self.functions[title] = Function(name=title, source="unknown")
        return title


def short_path(text: str) -> str:
    for prefix in (str(ROOT) + "/",):
        text = text.replace(prefix, "")
    return re.sub(r"^.*/esp-idf-v[0-9.]+/", "esp-idf/", text)


def merge(graph: Graph, title: str, function: Function) -> None:
    existing = graph.functions.get(title)
    if existing is None:
        graph.functions[title] = function
        return
    # A COMDAT or inline function compiled in several objects. Keep the
    # largest frame and every call any copy makes.
    if function.frame > existing.frame:
        existing.frame, existing.qualifier, existing.location = (
            function.frame, function.qualifier, function.location)
    existing.calls |= function.calls
    existing.indirect |= function.indirect


def load_ci(paths: list[Path]) -> Graph:
    graph = Graph()
    for path in paths:
        defined: dict[str, Function] = {}
        frameless: dict[str, Function] = {}
        edges: list[tuple[str, str]] = []
        for line in path.read_text(errors="replace").splitlines():
            if match := NODE_RE.match(line):
                title, label = match.groups()
                parts = label.split("\\n")
                frame = FRAME_RE.search(parts[-1]) if len(parts) >= 3 else None
                if frame:
                    # GCC prints a variadic method's name as ")", so keep the symbol.
                    name = parts[0] if parts[0].strip(") ") else title.rsplit(":", 1)[-1]
                    defined[title] = Function(
                        name=name, location=short_path(parts[1]),
                        frame=int(frame.group(1)), qualifier=frame.group(2))
                elif len(parts) == 2:
                    # A frameless node with a location is a definition GCC
                    # emitted as an alias, such as the C1 and D1 complete-object
                    # constructor and destructor that call their C2 and D2 body.
                    frameless[title] = Function(name=parts[0], location=short_path(parts[1]))
            elif match := EDGE_RE.match(line):
                edges.append(match.groups())
        for source, target in edges:
            function = defined.get(source)
            if function is None and source in frameless:
                # Kept only as an edge source, so a plain declaration stays unknown.
                function = defined[source] = frameless.pop(source)
            if function is None:
                continue
            if target == INDIRECT:
                function.indirect = True
            else:
                function.calls.add(target)
        for title, function in defined.items():
            merge(graph, title, function)
    return graph


def add_disassembly(graph: Graph, listing: str) -> None:
    """Frames and calls for linked code that has no .ci, from `objdump -d`."""
    # A static function's .ci title is `path:symbol`, its ELF symbol is bare.
    known = set(graph.functions) | {title.rsplit(":", 1)[1] for title in graph.functions if ":" in title}
    current: Function | None = None
    frame_seen = False
    for line in listing.splitlines():
        if match := FUNC_RE.match(line):
            name = match.group(2)
            current = None if name in known else Function(name=name, source="disassembly")
            if current is not None:
                graph.functions[name] = current
            frame_seen = False
            continue
        if current is None:
            continue
        if not frame_seen and (match := ENTRY_RE.search(line)):
            current.frame = int(match.group(1))
            frame_seen = True
        elif match := CALL_RE.search(line):
            current.calls.add(match.group(1))
        elif CALLX_RE.search(line):
            current.indirect = True


def pattern_matches(graph: Graph, pattern: str) -> list[str]:
    """A pattern is an fnmatch glob over the demangled signature or the symbol.
    `path-glob::glob` also requires the definition to sit in a matching file."""
    path_glob = None
    if "::" in pattern and not pattern.startswith("*") and "/" in pattern.split("::", 1)[0]:
        path_glob, pattern = pattern.split("::", 1)
    found = []
    for title, function in graph.functions.items():
        if function.source == "unknown":
            continue
        if path_glob and not fnmatch.fnmatchcase(function.location.split(":", 1)[0], path_glob):
            continue
        if fnmatch.fnmatchcase(function.name, pattern) or fnmatch.fnmatchcase(title, pattern):
            found.append(title)
    return sorted(found)


@dataclass
class TaskResult:
    worst: int
    path: list[tuple[str, int]]
    recursion: list[list[str]]
    pointer_cycles: int
    dynamic: list[str]
    unknown: list[str]
    unresolved_indirect: int


IndirectRule = tuple[frozenset[str] | None, list[str]]


def indirect_rules(graph: Graph, specs: list) -> list[IndirectRule]:
    """A plain pattern is a target any pointer call may reach. An object
    `{"from": [...], "to": [...]}` limits its targets to pointer calls made by
    the matching callers, so a library's own callbacks stay out of our paths."""
    rules: list[IndirectRule] = []
    loose: set[str] = set()
    for spec in specs:
        if isinstance(spec, str):
            loose.update(pattern_matches(graph, spec))
            continue
        callers = frozenset(title for pattern in spec["from"] for title in pattern_matches(graph, pattern))
        targets = sorted({title for pattern in spec["to"] for title in pattern_matches(graph, pattern)})
        rules.append((callers, targets))
    if loose:
        rules.insert(0, (None, sorted(loose)))
    return rules


def analyse(graph: Graph, entries: list[str], indirect: list[IndirectRule], depth: int) -> TaskResult:
    sys.setrecursionlimit(max(sys.getrecursionlimit(), 20000))
    # A key is a function and the pointer calls already taken on the path to
    # it. Without the cap every hook could reach every other hook and the
    # worst case would be a chain no event loop can run.
    Key = tuple[str, int]
    # A result cut short where a cycle closed above the function depends on
    # the path that reached it. It keeps the cycle heads it was cut at and is
    # reused only while all of them are on the path again, which cuts the
    # same cycles. On any other path it is computed again. A complete result
    # reused inside a cycle can count a frame twice, which errs high.
    memo: dict[Key, tuple[int, Key | None, frozenset[str]]] = {}
    on_stack: list[Key] = []
    on_stack_titles: set[str] = set()
    cycles: dict[frozenset[str], list[str]] = {}
    pointer_cycles: set[frozenset[str]] = set()
    dynamic: set[str] = set()
    unknown: set[str] = set()
    unresolved: set[str] = set()

    def callees(key: Key) -> list[Key]:
        title, level = key
        function = graph.functions[title]
        out = [(graph.resolve(target), level) for target in sorted(function.calls)]
        if function.indirect:
            targets = sorted({target for callers, rule_targets in indirect
                              if callers is None or title in callers for target in rule_targets})
            if targets and level < depth:
                out += [(target, level + 1) for target in targets]
            else:
                unresolved.add(title)
        return out

    def visit(key: Key) -> tuple[int, frozenset[str]]:
        """The worst cost below key and the functions above it where a cycle was cut."""
        if key in memo and memo[key][2] <= on_stack_titles:
            return memo[key][0], memo[key][2]
        function = graph.functions[key[0]]
        if function.source == "unknown":
            unknown.add(function.name)
        if function.qualifier.startswith("dynamic"):
            dynamic.add(f"{display(function)} ({function.qualifier}, {function.frame} B)")
        on_stack.append(key)
        on_stack_titles.add(key[0])
        best, best_key, heads = 0, None, set()
        for child in callees(key):
            # A function already on the path closes a cycle. Direct recursion
            # is listed. A cycle closed through a pointer call is usually the
            # pattern over-approximation (a library calling back into itself
            # through a hook), so it is only counted.
            if child[0] in on_stack_titles:
                start = next(i for i, (name, _) in enumerate(on_stack) if name == child[0])
                heads.add(child[0])
                names = sorted({display(graph.functions[name]) for name, _ in on_stack[start:]})
                if on_stack[start] == child:
                    cycles.setdefault(frozenset(names), names)
                else:
                    pointer_cycles.add(frozenset(names))
                continue
            cost, child_heads = visit(child)
            heads |= child_heads
            if cost > best:
                best, best_key = cost, child
        on_stack.pop()
        on_stack_titles.discard(key[0])
        total = function.frame + best
        heads.discard(key[0])
        memo[key] = (total, best_key, frozenset(heads))
        return total, memo[key][2]

    worst, start = 0, None
    for entry in entries:
        cost, _ = visit((entry, 0))
        if cost > worst or start is None:
            worst, start = cost, (entry, 0)
    # A result recomputed on a later path replaces the one an earlier caller
    # chose, so the printed chain stops where it would repeat a function.
    path, titles = [], set()
    key = start
    while key is not None and key[0] not in titles:
        titles.add(key[0])
        function = graph.functions[key[0]]
        path.append((display(function), function.frame))
        key = memo[key][1]
    return TaskResult(worst, path, sorted(cycles.values()), len(pointer_cycles), sorted(dynamic), sorted(unknown),
                      len(unresolved))


def display(function: Function) -> str:
    name = function.name
    if function.location:
        name += f" [{function.location.rsplit(':', 1)[0]}]"
    return name


def elf_sections(path: Path) -> dict[str, int]:
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise BudgetError(f"{path} is not a 32-bit little-endian ELF")
    offset = struct.unpack_from("<I", data, 0x20)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 0x2E)
    headers = [struct.unpack_from("<10I", data, offset + index * size) for index in range(count)]
    names = headers[names_index][4]
    sections = {}
    for header in headers:
        if header[2] & 2 and header[5]:  # SHF_ALLOC and not empty
            start = names + header[0]
            sections[data[start:data.index(b"\0", start)].decode()] = header[5]
    return sections


def sdkconfig(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "sdkconfig").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip('"')
    return values


def objdump_path(build: Path) -> Path | None:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith("CMAKE_OBJDUMP:FILEPATH="):
                tool = Path(line.split("=", 1)[1])
                return tool if tool.is_file() else None
    return None


@dataclass
class ImageReport:
    name: str
    tasks: dict[str, dict]
    sections: dict[str, int]
    stack_memory: list[str] = field(default_factory=list)
    stack_memory_checked: bool = True
    task_sites: list[str] = field(default_factory=list)
    scrubs: list[str] = field(default_factory=list)
    scrub_findings: list[str] = field(default_factory=list)


# NVS and SPI flash writes disable the cache, so a task that writes NVS must
# run on an internal RAM stack. xTaskCreate always allocates its stack from
# internal RAM. CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY (an ESP-IDF default
# with PSRAM) only lets the calls below take a PSRAM stack, so any of them in
# repo firmware fails the check until it is reviewed. Comments are skipped.
# There is no allowlist, so a reviewed internal RAM static task means
# extending this check rather than silencing it.
EXTERNAL_STACK_CONFIG = "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY"
EXTERNAL_STACK_RE = re.compile(r"\b(xTaskCreateWithCaps|xTaskCreatePinnedToCoreWithCaps|xTaskCreateStatic"
                               r"|xTaskCreateStaticPinnedToCore|xTaskCreateRestrictedStatic)\s*\(")
# xTaskCreate(entry, "name", stack, ...). The stack is a constant expression
# or a member whose single default the scan finds in the same sources. Every
# other xTaskCreate* call (a lambda entry, a name in a variable, a Static or
# WithCaps variant, a call split by a macro) is kept as an unparsed site and
# fails in any image that links its file, so the scan fails closed.
TASK_TOKEN_RE = re.compile(r"\bxTaskCreate\w*\s*\(")
TASK_CREATE_RE = re.compile(r'\bxTaskCreate(?:PinnedToCore)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*"([^"]*)"\s*,\s*([^,]+?)\s*,')
CONSTANT_RE = re.compile(r"^[0-9\s*+()]+$")
# Comments, literals, identifiers and numbers in one pass, so none hides
# another. A raw string R"d(...)d" ends only at its own delimiter. Identifiers
# and preprocessing numbers are matched whole and kept, so a quote inside a
# number such as 4'096 is a digit separator, and a quote right after any word
# (a keyword as in case'"', or an encoding prefix as in L'"') opens a
# character literal. Line splices come before comments in C, so a line
# comment ending in a backslash goes on over the next line.
LEXEME_RE = re.compile(r'(?P<raw>(?<!\w)(?:u8|[uUL])?R"(?P<delim>[^()\\\s"]{0,16})\(.*?\)(?P=delim)")'
                       r"|(?P<word>\.?\d(?:[eEpP][+-]|'\w|[\w.])*|[A-Za-z_]\w*)"
                       r'|(?P<string>"(?:\\.|[^"\\\n])*")'
                       r"|(?P<char>'(?:\\.|[^'\\\n])*')"
                       r"|(?P<comment>//(?:\\\r?\n|[^\n])*|/\*.*?\*/)", re.S)
# A conditional directive opens, continues or closes a group of branches.
DIRECTIVE_RE = re.compile(r"[ \t]*#[ \t]*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b")
SOURCE_SKIP = {"build", "managed_components", "vendor"}


@dataclass
class TaskSite:
    where: str  # path:line
    path: str
    entry: str
    name: str
    stack: int | None
    stack_text: str
    parsed: bool = True
    # The (group, branch) pairs of every #if around the call, see conditional_branches.
    branches: tuple[tuple[str, int], ...] = ()


@dataclass
class SourceScan:
    external: list[str] = field(default_factory=list)
    sites: list[TaskSite] = field(default_factory=list)


def strip_comments(text: str) -> str:
    """Blank out comments and keep string literals and line numbers."""
    def blank(match: re.Match) -> str:
        token = match.group(0)
        return re.sub(r"[^\n]", " ", token) if match.group("comment") else token
    return LEXEME_RE.sub(blank, text)


def blank_literals(text: str) -> str:
    """Blank the inside of string and character literals, keeping any prefix,
    the quotes, the length and line numbers, so call names in log text are not
    calls."""
    def blank(match: re.Match) -> str:
        token = match.group(0)
        if not (match.group("raw") or match.group("string") or match.group("char")):
            return token
        start = min(i for i in (token.find('"'), token.find("'")) if i >= 0) + 1
        return token[:start] + re.sub(r"[^\n]", " ", token[start:-1]) + token[-1]
    return LEXEME_RE.sub(blank, text)


def conditional_branches(code: str, path: str) -> list[tuple[tuple[str, int], ...]]:
    """For each line, the `#if` groups it sits in, outermost first, as (group,
    branch) pairs. A group is named by the path and line of its `#if`, and
    each `#elif` or `#else` moves to the next branch. Comments and literals
    must already be blank, so a directive in a raw string is not one. The
    conditions are never evaluated, so every branch is scanned."""
    stack: list[tuple[str, int]] = []
    lines = []
    continued = False
    for number, line in enumerate(code.split("\n"), 1):
        directive = None if continued else DIRECTIVE_RE.match(line)
        continued = line.rstrip().endswith("\\")
        if directive and directive.group(1).startswith("if"):
            stack.append((f"{path}:{number}", 0))
        elif directive and directive.group(1) == "endif":
            stack = stack[:-1]
        elif directive and stack:
            stack[-1] = (stack[-1][0], stack[-1][1] + 1)
        lines.append(tuple(stack))
    return lines


def exclusive(a: TaskSite, b: TaskSite) -> bool:
    """True when the two sites sit in different branches of one `#if` group,
    so at most one of them is compiled."""
    branches = dict(a.branches)
    return any(group in branches and branches[group] != branch for group, branch in b.branches)


def constant(text: str) -> int | None:
    """Digits, spaces, `*`, `+` and parentheses only, such as `16 * 1024`."""
    def value(node: ast.expr) -> int:
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Mult, ast.Add)):
            left, right = value(node.left), value(node.right)
            return left * right if isinstance(node.op, ast.Mult) else left + right
        raise ValueError(ast.dump(node))

    if not CONSTANT_RE.match(text.strip()):
        return None
    try:
        return value(ast.parse(text.strip(), mode="eval").body)
    except (SyntaxError, ValueError):
        return None


def scan_sources(root: Path) -> SourceScan:
    texts = {}
    for path in sorted((root / "firmware").rglob("*")):
        relative = path.relative_to(root)
        if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"} and not SOURCE_SKIP & set(relative.parts):
            texts[relative.as_posix()] = strip_comments(path.read_text(errors="replace"))
    scan = SourceScan()
    for name, text in texts.items():
        # Calls are found in code only. The literal text at the same offsets
        # still gives the task name.
        code = blank_literals(text)
        branches = conditional_branches(code, name)
        for match in EXTERNAL_STACK_RE.finditer(code):
            scan.external.append(f"{name}:{text.count(chr(10), 0, match.start()) + 1}")
        for token in TASK_TOKEN_RE.finditer(code):
            line = text.count(chr(10), 0, token.start()) + 1
            where = f"{name}:{line}"
            if not (match := TASK_CREATE_RE.match(text, token.start())):
                call = " ".join(text[token.start():token.start() + 80].split(";")[0].split())
                scan.sites.append(TaskSite(where, name, "", "", None, call, parsed=False,
                                           branches=branches[line - 1]))
                continue
            entry, task, stack_text = match.groups()
            scan.sites.append(TaskSite(where, name, entry, task, constant(stack_text), " ".join(stack_text.split()),
                                       branches=branches[line - 1]))
    for site in scan.sites:
        if site.stack is None and (words := re.findall(r"[A-Za-z_]\w*", site.stack_text)):
            member = re.compile(rf"\b{words[-1]}\s*(?:=|\{{)\s*([0-9\s*+()]+?)\s*[;}},]")
            values = {constant(match.group(1)) for text in texts.values() for match in member.finditer(text)}
            site.stack = values.pop() if len(values) == 1 else None
    return scan


def external_stack_sources(root: Path) -> list[str]:
    return scan_sources(root).external


def stack_memory_findings(config: dict[str, str], sources: list[str]) -> list[str]:
    if config.get(EXTERNAL_STACK_CONFIG) != "y":
        return []  # A PSRAM stack buffer would fail at task creation instead.
    return [f"{where} creates a task with a caller-chosen stack, check it is internal RAM" for where in sources]


def task_site_findings(graph: Graph, spec: dict, budgets: dict, sites: list[TaskSite]) -> list[str]:
    """Every xTaskCreate in a file this image links names a tracked task, and
    its stack in source matches the copied `stack_bytes`, so neither can
    drift. A file counts as linked when any of its functions is in the graph,
    whatever file the entry lives in."""
    linked_files = {function.location.split(":", 1)[0] for function in graph.functions.values()
                    if function.location}

    names = {task_name: image_task.get("task_name", budgets["tasks"][task_name].get("task_name", task_name))
             for task_name, image_task in spec["tasks"].items()}
    here = [site for site in sites if site.path in linked_files]
    findings = []
    for site in here:
        if not site.parsed:
            findings.append(f"{site.where} creates a task the scan cannot parse ({site.stack_text}). "
                            'Use xTaskCreate(entry, "literal name", stack, ...) with a named entry')
            continue
        tracked = [task_name for task_name, name in names.items() if name == site.name]
        if not tracked:
            findings.append(f'{site.where} creates task "{site.name}", which no task in this image tracks. '
                            "Add it to tools/stack_budget.json")
            continue
        copied = spec["tasks"][tracked[0]].get("stack_bytes")
        if site.stack is None:
            findings.append(f'{site.where} creates "{site.name}" with a stack the scan cannot read '
                            f"({site.stack_text}). Use a constant or a member with one default")
        elif copied != site.stack:
            findings.append(f"{tracked[0]} stack_bytes {copied} does not match the {site.stack} B "
                            f"stack at {site.where}")
    # One creation site per tracked name. A second copy takes its own stack
    # from the heap, which no budget here counts. Sites in different branches
    # of one #if group are alternatives, so only a pair that can be compiled
    # together counts. Keys that share a task name report it once.
    for name in sorted(set(names.values())):
        copies = [site for site in here if site.parsed and site.name == name]
        if any(not exclusive(a, b) for i, a in enumerate(copies) for b in copies[i + 1:]):
            findings.append(f'task "{name}" is created at {len(copies)} sites '
                            f'({", ".join(site.where for site in copies)}). Keep one creation site per tracked task')
    created = {site.name for site in here}
    for task_name, image_task in spec["tasks"].items():
        if "stack_bytes" in image_task and names[task_name] not in created:
            findings.append(f'{task_name} has stack_bytes but this image creates no task "{names[task_name]}"')
    return findings


def scrub_check(graph: Graph, task_name: str, scrub: dict,
                indirect: list[IndirectRule], depth: int) -> tuple[str | None, list[str]]:
    """A summary line, and a finding for each way the scrub fails to cover."""
    titles = pattern_matches(graph, scrub["function"])
    if len(titles) != 1:
        return None, [f"{task_name}: the scrub pattern {scrub['function']} matches {len(titles)} functions, "
                      "it must match one"]
    frame = graph.functions[titles[0]].frame
    margin = int(scrub.get("margin_bytes", 0))
    findings, deepest = [], 0
    for pattern in scrub["covers"]:
        covered = pattern_matches(graph, pattern)
        if not covered:
            findings.append(f"{task_name}: the scrub cover {pattern} matches nothing, update the pattern "
                            "if the function was renamed or inlined")
            continue
        worst = analyse(graph, covered, indirect, depth).worst
        deepest = max(deepest, worst)
        if worst + margin > frame:
            findings.append(f"{task_name}: {pattern} uses {worst} B below its caller, past the {frame} B "
                            f"scrub frame less its {margin} B margin")
    return (f"{task_name} scrub frame {frame} B covers {deepest} B with {margin} B margin", findings)


def measure_image(name: str, spec: dict, budgets: dict, require_objdump: bool,
                  scan: SourceScan | None = None) -> ImageReport:
    build = ROOT / spec["build"]
    description = build / "project_description.json"
    if not description.is_file():
        raise BudgetError(f"{name}: no build in {spec['build']}, run the board build first")
    elf = build / json.loads(description.read_text())["app_elf"]
    ci_files = sorted(build.rglob("*.ci"))
    if not ci_files:
        raise BudgetError(f"{name}: no .ci call graphs in {spec['build']}, rebuild with os_enable_stack_usage()")
    graph = load_ci(ci_files)
    tool = objdump_path(build)
    if tool is None:
        if require_objdump:
            raise BudgetError(f"{name}: the toolchain objdump named in CMakeCache.txt is missing")
        print(f"warning: {name}: no objdump, prebuilt library frames count as zero", file=sys.stderr)
    else:
        listing = subprocess.run([str(tool), "-d", "--no-show-raw-insn", str(elf)], check=True,
                                 capture_output=True, text=True).stdout
        add_disassembly(graph, listing)
    config = sdkconfig(build)
    scan = scan or SourceScan()
    sites = task_site_findings(graph, spec, budgets, scan.sites)
    tasks = {}
    scrubs: list[str] = []
    scrub_findings: list[str] = []
    for task_name, image_task in spec["tasks"].items():
        task = budgets["tasks"][task_name]
        entries = [title for pattern in image_task.get("entry", task.get("entry", []))
                   for title in pattern_matches(graph, pattern)]
        if not entries:
            raise BudgetError(f"{name}: no function matches the {task_name} entry {task['entry']}")
        indirect = indirect_rules(graph, budgets.get("shared_indirect", []) + task.get("indirect", [])
                                  + image_task.get("indirect", []))
        result = analyse(graph, entries, indirect, int(task.get("indirect_depth", 2)))
        if "scrub" in task:
            line, findings = scrub_check(graph, task_name, task["scrub"], indirect,
                                         int(task.get("indirect_depth", 2)))
            scrubs += [line] if line else []
            scrub_findings += findings
        stack = image_task.get("stack_bytes")
        if stack is None and "stack_sdkconfig" in task:
            stack = int(config.get(task["stack_sdkconfig"], "0")) or None
        tasks[task_name] = {"result": result, "stack": stack, "indirect_targets": len({t for _, targets in indirect for t in targets})}
    sections = elf_sections(elf)
    return ImageReport(name, tasks, sections, stack_memory_findings(config, scan.external),
                       config.get(EXTERNAL_STACK_CONFIG) == "y", sites, scrubs, scrub_findings)


def headroom(value: int, percent: float, step: int) -> int:
    return int(math.ceil(value * (1 + percent / 100) / step) * step)


def kib(value: int) -> str:
    return f"{value:,}"


def render(report: ImageReport, spec: dict, budgets: dict, show_path: bool) -> tuple[list[str], list[str]]:
    reserve = int(budgets["policy"]["reserve_bytes"])
    lines = [f"## {report.name}", "",
             "| Task | Stack | Static worst | Plus reserve | Margin | Budget | Flags |",
             "| --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    failures = []
    for task_name, measured in report.tasks.items():
        result: TaskResult = measured["result"]
        stack = measured["stack"]
        budget = spec["tasks"][task_name].get("budget")
        flags = []
        if result.recursion:
            flags.append(f"recursion x{len(result.recursion)}")
        if result.pointer_cycles:
            flags.append(f"pointer cycle x{result.pointer_cycles}")
        if any("(dynamic,bounded" not in item for item in result.dynamic):
            flags.append("unbounded frame")
        elif result.dynamic:
            flags.append("bounded dynamic frame")
        if result.unresolved_indirect:
            flags.append(f"unresolved indirect x{result.unresolved_indirect}")
        with_reserve = result.worst + reserve
        margin = "" if stack is None else kib(stack - with_reserve)
        if stack is not None and with_reserve > stack:
            flags.append("OVER STACK")
            failures.append(f"{report.name} {task_name}: static worst {result.worst} B plus the "
                            f"{reserve} B reserve is past the {stack} B stack")
        if budget is None:
            flags.append("NO BUDGET")
            failures.append(f"{report.name} {task_name}: no budget, review it and run `pixi run stack-budget "
                            f"--write-budgets --image {report.name}`")
        elif result.worst > budget:
            flags.append("OVER BUDGET")
            failures.append(f"{report.name} {task_name}: static worst {result.worst} B is past "
                            f"its budget {budget} B")
        lines.append(f"| {task_name} | {'' if stack is None else kib(stack)} | {kib(result.worst)} | "
                     f"{kib(with_reserve)} | {margin} | {'' if budget is None else kib(budget)} | "
                     f"{', '.join(flags)} |")
    if report.task_sites:
        failures += [f"{report.name} task source: {item}" for item in report.task_sites]
        lines += ["", *[f"Task source check failed. {item}." for item in report.task_sites]]
    else:
        lines += ["", "Task source check passed. Every xTaskCreate in a file this image links is tracked with its "
                  "stack size."]
    if report.scrub_findings:
        failures += [f"{report.name} scrub: {item}" for item in report.scrub_findings]
        lines += ["", *[f"Scrub check failed. {item}." for item in report.scrub_findings]]
    elif report.scrubs:
        lines += ["", *[f"Scrub check passed. {item}." for item in report.scrubs]]
    if report.stack_memory:
        failures += [f"{report.name} internal stack: {item}" for item in report.stack_memory]
        lines += ["", *[f"Internal stack check failed. {item}." for item in report.stack_memory]]
    elif report.stack_memory_checked:
        lines += ["", "Internal stack check passed. Repo firmware creates every task with xTaskCreate, so "
                      "every task stack (the NVS writers included) is internal RAM."]
    else:
        lines += ["", f"Internal stack check skipped. {EXTERNAL_STACK_CONFIG} is off in this image, so no "
                      "task stack can be in PSRAM."]
    lines += ["", "| Section | Bytes | Budget |", "| --- | ---: | ---: |"]
    for section, budget in spec.get("sections", {}).items():
        used = report.sections.get(section, 0)
        mark = ""
        if used > budget:
            mark = " OVER BUDGET"
            failures.append(f"{report.name} {section}: {used} B is past its budget {budget} B")
        lines.append(f"| {section} | {kib(used)} | {kib(budget)}{mark} |")
    for task_name, measured in report.tasks.items():
        result = measured["result"]
        details = []
        if show_path:
            details += [f"    {frame:>6}  {name}" for name, frame in result.path]
        details += [f"    recursion: {' -> '.join(cycle)}" for cycle in result.recursion]
        details += [f"    dynamic frame: {item}" for item in result.dynamic]
        if show_path and result.unknown:
            details.append(f"    no frame data ({len(result.unknown)}): {', '.join(result.unknown[:12])}"
                           + (" ..." if len(result.unknown) > 12 else ""))
        if details:
            lines += ["", f"{task_name}, {measured['indirect_targets']} indirect targets, "
                          f"{len(result.path)} frames deep", "", "```", *details, "```"]
    return lines, failures


def write_budgets(path: Path, budgets: dict, reports: list[ImageReport]) -> None:
    policy = budgets["policy"]
    reserve = int(policy["reserve_bytes"])
    for report in reports:
        spec = budgets["images"][report.name]
        for task_name, measured in report.tasks.items():
            worst = measured["result"].worst
            budget = headroom(worst, policy["stack_headroom_percent"], policy["stack_round_bytes"])
            # Headroom never reaches past the task stack minus the reserve, so a
            # task that is already tight keeps the little room it has.
            if measured["stack"] is not None:
                budget = max(worst, min(budget, measured["stack"] - reserve))
            spec["tasks"][task_name]["measured"] = worst
            spec["tasks"][task_name]["budget"] = budget
        spec["sections_measured"] = {name: report.sections.get(name, 0) for name in spec["sections"]}
        spec["sections"] = {name: headroom(report.sections.get(name, 0), policy["section_headroom_percent"],
                                           policy["section_round_bytes"]) for name in spec["sections"]}
    path.write_text(json.dumps(budgets, indent=2) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", action="append", help="board/variant, repeatable (default every built image)")
    parser.add_argument("--budgets", type=Path, default=DEFAULT_BUDGETS)
    parser.add_argument("--check", action="store_true", help="exit 1 when a task or section is past its budget")
    parser.add_argument("--write-budgets", action="store_true",
                        help="rewrite measured values and budgets with the policy headroom")
    parser.add_argument("--path", action="store_true", help="print each task's deepest call path")
    args = parser.parse_args(argv)
    budgets = json.loads(args.budgets.read_text())
    names = args.image or [name for name, spec in budgets["images"].items()
                           if (ROOT / spec["build"] / "project_description.json").is_file()]
    if not names:
        print("error: no board build found, run cores3-build or waveshare-build first", file=sys.stderr)
        return 2
    reports, failures, output = [], [], []
    scan = scan_sources(ROOT)
    try:
        for name in names:
            if name not in budgets["images"]:
                raise BudgetError(f"unknown image {name}, known: {', '.join(budgets['images'])}")
            spec = budgets["images"][name]
            report = measure_image(name, spec, budgets, require_objdump=args.check or args.write_budgets,
                                   scan=scan)
            reports.append(report)
            lines, image_failures = render(report, spec, budgets, args.path)
            output += lines + [""]
            failures += image_failures
    except BudgetError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    policy = budgets["policy"]
    print(f"# Stack and section budgets\n\nStatic worst case in bytes from GCC call graphs. "
          f"Plus reserve adds {policy['reserve_bytes']} B for the interrupt frame and window spills. "
          f"Budgets allow {policy['stack_headroom_percent']} % on stacks and "
          f"{policy['section_headroom_percent']} % on sections over the committed measurement.\n")
    print("\n".join(output))
    if args.write_budgets:
        if stale := [item for report in reports for item in report.task_sites]:
            print("error: fix the task source findings before writing budgets, the stack cap reads stack_bytes",
                  file=sys.stderr)
            for item in stale:
                print(f"  {item}", file=sys.stderr)
            return 2
        write_budgets(args.budgets, budgets, reports)
        print(f"wrote {args.budgets.relative_to(ROOT) if args.budgets.is_relative_to(ROOT) else args.budgets}")
        return 0
    if failures:
        print("Budget check failed. If the growth is intended, review it and run "
              "`pixi run stack-budget --write-budgets` for the affected image.", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1 if args.check else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
