"""Build and run the shared text component tests on the host."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    src = root / "firmware/common/text/src"
    with tempfile.TemporaryDirectory(prefix="os-text-") as directory:
        executable = Path(directory) / "text-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_text.cpp"),
                   str(src / "os_text.cpp"),
                   str(src / "os_text_font.cpp"),
                   str(src / "os_arabic_fonts_s_gen.cpp"),
                   str(src / "os_arabic_fonts_l_gen.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("UTF-8 decoding, Arabic shaping, bidi order, the string table and "
          "baked glyph coverage passed")


if __name__ == "__main__":
    main()
