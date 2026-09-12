#!/usr/bin/env python3
"""Build-time workaround for the base Windows SDK 10.0.26100.0 XAML compiler bug.

Magpie cannot be compiled with the 26100 RTM XAML compiler (WMC9999 crash), so the
build redirects the XAML compiler to the older 19041 SDK's (non-buggy) compiler.
That older compiler, however, emits *conflicting* definitions for some x:Bind helper
templates (e.g. Get/SetReferenceTypeMember_Header): it generates BOTH a generic
variant (box_value / value.as<TValue>()) AND a hard-coded string variant
(PropertyValue::CreateString / unbox_value<winrt::hstring>). Those share the same
signature, producing C2995 "function template already defined".

This script removes the hard-coded string variants and keeps the generic one, which
is correct for any property type. This is a build-time workaround only; the real fix
is a patched Windows SDK 10.0.26100.x (shipped with VS 2022 17.14.10+).
"""
import sys
import re
import os


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


# Markers that identify the broken, hard-coded-string variants the old compiler
# emits alongside the correct generic variant.
BAD_MARKERS = ("PropertyValue::CreateString", "unbox_value<::winrt::hstring>")


def dedupe_file(path):
    if not os.path.isfile(path):
        print("skip (not found):", path)
        return False
    with open(path, "r", encoding="utf-8-sig", errors="ignore") as f:
        text = f.read()
    n = len(text)

    blocks = []  # (start, end, signature, full_block)
    for m in re.finditer(r"template", text):
        j = m.start()
        # only consider namespace-scope (brace depth == 1) template definitions
        depth = text[:j].count("{") - text[:j].count("}")
        if depth != 1:
            continue
        k = text.find("{", j)
        if k == -1:
            continue
        dd = 0
        p2 = k
        while p2 < n:
            c = text[p2]
            if c == "{":
                dd += 1
            elif c == "}":
                dd -= 1
                if dd == 0:
                    break
            p2 += 1
        blk = text[j : p2 + 1]
        try:
            sig = norm(blk[: blk.index("{")])
        except ValueError:
            continue
        blocks.append([j, p2 + 1, sig, blk])

    from collections import OrderedDict

    groups = OrderedDict()
    for b in blocks:
        groups.setdefault(b[2], []).append(b)

    to_remove = []
    for sig, items in groups.items():
        if len(items) <= 1:
            continue
        good = [b for b in items if not any(mk in b[3] for mk in BAD_MARKERS)]
        if good:
            keep = good[0]
        else:
            keep = items[0]
        for b in items:
            if b is not keep:
                to_remove.append(b)

    if not to_remove:
        print("no conflicts in:", path)
        return False

    for b in sorted(to_remove, key=lambda x: x[0], reverse=True):
        start = b[0]
        # trim a single leading blank line so we don't leave a double gap
        while start > 0 and text[start - 1] in "\r\n":
            start -= 1
            if start > 0 and text[start - 1] in "\r\n":
                break
        text = text[:start] + text[b[1]:]
    text = re.sub(r"\n{3,}", "\n\n", text)

    with open(path, "w", encoding="utf-8-sig") as f:
        f.write(text)
    print("removed %d conflicting/duplicate template block(s) in: %s" % (len(to_remove), path))
    return True


def main():
    if len(sys.argv) > 1:
        paths = sys.argv[1:]
    else:
        base = r"C:\Users\wuyua\Project\Magpie\obj\x64\Release\Magpie\Generated Files"
        paths = [os.path.join(base, "XamlTypeInfo.g.cpp")]
    # This is a build-time workaround hook. As long as the script runs without
    # raising (i.e. the file exists and was processed, or didn't need changes,
    # or was absent), it must exit 0 so it can never abort the MSBuild build.
    # Only an unexpected exception would surface as a non-zero exit.
    try:
        for p in paths:
            dedupe_file(p)
    except Exception as e:  # noqa: BLE001 - last-resort guard for the build
        print("error processing %s: %s" % (paths, e))
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
