"""Sanity-check that the generated vectors really contain the condition they are named after."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
V = os.path.join(HERE, "vectors")


def text(name):
    return open(os.path.join(V, name + ".step"), encoding="latin-1", newline="").read()


t = text("ok_items_reordered")
order = [int(m) for m in re.findall(r"DESCRIPTIVE_REPRESENTATION_ITEM\('NCX\.(\d+)'", t)][:4]
print("reordered file order of first chunks:", order)
t = text("ok_root_preferred")
print("NCX property definitions:", len(re.findall(r"PROPERTY_DEFINITION\('NCX'", t)))
t = text("chunk_missing")
print("NCX.0003 present:", "'NCX.0003'" in t)
t = text("ok_wrapped_string")
m = re.search(r"'NCX\.0001','([^']*)'", t, re.S)
print("NCX.0001 value spans lines:", "\n" in m.group(1))
t = text("ok_lf_only")
print("CRLF present in lf_only:", "\r\n" in t)
t = text("schema_too_new")
print("schema field:", re.search(r"schema=([0-9.]+)", t).group(1))
