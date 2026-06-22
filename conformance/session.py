"""
Environment discovery shared by run.py and test_conformance.py: where is the
`qwc` binary, is `wc` present, and which locales can we exercise.
"""

from __future__ import annotations

import dataclasses
import os
import shutil
import subprocess
from typing import Optional


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Locale candidates to probe for a working multibyte (UTF-8) locale. The bare
# "UTF-8" entry is the macOS form; the others are the usual Linux names.
UTF8_LOCALE_CANDIDATES = (
    os.environ.get("QWC_CONF_UTF8_LOCALE", ""),
    "C.UTF-8",
    "C.utf8",
    "en_US.UTF-8",
    "en_US.utf8",
    "UTF-8",
)


@dataclasses.dataclass(frozen=True)
class Session:
    qwc_bin: str
    c_locale: str               # always "C"
    utf8_locale: Optional[str]  # None if no multibyte locale is available

    def regimes(self) -> list[tuple[str, str]]:
        """(regime-name, locale) pairs to run every case under."""
        out = [("C", self.c_locale)]
        if self.utf8_locale is not None:
            out.append(("UTF8", self.utf8_locale))
        return out


def find_qwc() -> str:
    """Locate the qwc binary: $QWC_BIN, then the repo root, then $PATH."""
    env = os.environ.get("QWC_BIN")
    if env:
        if not os.path.isfile(env):
            raise SystemExit(f"QWC_BIN={env!r} is not a file")
        return os.path.abspath(env)
    for candidate in (
        os.path.join(REPO_ROOT, "qwc"),
        os.path.join(REPO_ROOT, "build", "qwc"),
    ):
        if os.path.isfile(candidate):
            return candidate
    found = shutil.which("qwc")
    if found:
        return found
    raise SystemExit(
        "Could not find the qwc binary. Build it first "
        "(cmake --build <dir> --target qwc) or set QWC_BIN."
    )


def _wc_present() -> None:
    if shutil.which("wc") is None:
        raise SystemExit("System `wc` not found on PATH; it is the oracle.")


def _wc_char_count(text: bytes, locale: str) -> Optional[int]:
    """Return `wc -m` for *text* under *locale*, or None on failure."""
    env = dict(os.environ)
    env["LC_ALL"] = locale
    env["LANG"] = locale
    try:
        proc = subprocess.run(
            ["wc", "-m"], input=text, capture_output=True, env=env
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    toks = proc.stdout.split()
    return int(toks[0]) if toks and toks[0].isdigit() else None


def pick_utf8_locale() -> Optional[str]:
    """
    Find a locale in which `wc` actually does multibyte counting. We probe
    functionally rather than trusting names: feed the 2-byte sequence for "é"
    and accept the locale only if `wc -m` reports 1 character, not 2 bytes.
    """
    two_byte_e = b"\xc3\xa9"  # U+00E9, one character / two bytes
    for cand in UTF8_LOCALE_CANDIDATES:
        if not cand:
            continue
        if _wc_char_count(two_byte_e, cand) == 1:
            return cand
    return None


def build_session() -> Session:
    _wc_present()
    qwc_bin = find_qwc()
    utf8 = None if os.environ.get("QWC_CONF_NO_UTF8") else pick_utf8_locale()
    return Session(
        qwc_bin=qwc_bin,
        c_locale="C",
        utf8_locale=utf8,
    )
