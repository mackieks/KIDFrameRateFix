"""Dump a UE3 Kismet graph from a decompressed Killer Is Dead map.

Reads tagged properties directly from export serial data, using the name and
export tables written by DumpKidPackageObjects (<map>.objects.txt).

usage: py -3 DumpKismet.py <map.objects.txt> <map.decompressed.umap> [filter]

Prints one block per sequence object under Main_Sequence: its properties,
its output links (output -> target op/input) and its variable links.
"""
import re
import struct
import sys

STRUCT_ARRAYS = {"OutputLinks", "InputLinks", "VariableLinks", "EventLinks", "Links",
                 "GroupInst", "InterpTracks", "ConnectorPos"}


def load_tables(path):
    exports, names = {}, {}
    section = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("EXPORTS\t"):
                section = "exports"
                continue
            if line.startswith("NAMES\t"):
                section = "names"
                continue
            if line == "RELEVANT OBJECTS":
                break
            fields = line.split("\t")
            if not fields[0].isdigit():
                continue
            if section == "names" and len(fields) >= 2:
                names[int(fields[0])] = fields[1]
            elif section == "exports" and len(fields) >= 7:
                exports[int(fields[0])] = {"name": fields[1], "offset": int(fields[4]),
                                           "size": int(fields[5]), "path": fields[6]}
    return exports, names


class Reader:
    def __init__(self, data, names, exports):
        self.data, self.names, self.exports = data, names, exports

    def name(self, pos):
        index, number = struct.unpack_from("<ii", self.data, pos)
        text = self.names.get(index, "<name%d>" % index)
        return (text if number == 0 else "%s_%d" % (text, number - 1)), pos + 8

    def objref(self, value):
        if value > 0 and value - 1 in self.exports:
            return self.exports[value - 1]["path"].replace("TheWorld.PersistentLevel.", "")
        if value < 0:
            return "import%d" % (-value)
        return "None"

    def properties(self, pos, end, depth=0):
        out = []
        while pos + 8 <= end:
            name, pos = self.name(pos)
            if name == "None":
                return out, pos
            ptype, pos = self.name(pos)
            size, array_index = struct.unpack_from("<ii", self.data, pos)
            pos += 8
            extra = None
            if ptype == "StructProperty":
                extra, pos = self.name(pos)
            elif ptype == "BoolProperty":
                extra = self.data[pos]
                pos += 1
            elif ptype == "ByteProperty":
                extra, pos = self.name(pos)
            value_pos = pos
            pos += size
            out.append(self.decode(name, ptype, size, array_index, extra, value_pos, depth))
        return out, pos

    def decode(self, name, ptype, size, array_index, extra, pos, depth):
        label = name if array_index == 0 else "%s[%d]" % (name, array_index)
        data = self.data
        if ptype == "BoolProperty":
            return (label, bool(extra))
        if ptype == "IntProperty":
            return (label, struct.unpack_from("<i", data, pos)[0])
        if ptype == "FloatProperty":
            return (label, round(struct.unpack_from("<f", data, pos)[0], 4))
        if ptype in ("ObjectProperty", "ComponentProperty", "ClassProperty", "InterfaceProperty"):
            return (label, self.objref(struct.unpack_from("<i", data, pos)[0]))
        if ptype == "NameProperty":
            return (label, self.name(pos)[0])
        if ptype == "ByteProperty":
            if size == 1:
                return (label, data[pos])
            return (label, self.name(pos)[0])
        if ptype == "StrProperty":
            length = struct.unpack_from("<i", data, pos)[0]
            if length < 0:
                return (label, data[pos + 4:pos + 4 - 2 * length - 2].decode("utf-16-le", "replace"))
            return (label, data[pos + 4:pos + 4 + length - 1].decode("latin-1"))
        if ptype == "StructProperty":
            if extra == "Vector" and size == 12:
                return (label, tuple(round(v, 2) for v in struct.unpack_from("<fff", data, pos)))
            if extra == "Rotator" and size == 12:
                return (label, struct.unpack_from("<iii", data, pos))
            if size >= 8 and depth < 4:
                try:
                    inner, _ = self.properties(pos, pos + size, depth + 1)
                    return (label, inner)
                except (struct.error, KeyError):
                    pass
            return (label, "<%s %d bytes>" % (extra, size))
        if ptype == "ArrayProperty":
            count = struct.unpack_from("<i", data, pos)[0]
            body = pos + 4
            if name in STRUCT_ARRAYS and depth < 4:
                items = []
                for _ in range(count):
                    item, body = self.properties(body, pos + size, depth + 1)
                    items.append(item)
                return (label, items)
            if size == 4 + 4 * count:
                values = struct.unpack_from("<%di" % count, data, body)
                return (label, [self.objref(v) for v in values])
            return (label, "<array %d items, %d bytes>" % (count, size))
        return (label, "<%s %d bytes>" % (ptype, size))


def fmt(value, indent=""):
    if isinstance(value, list) and value and isinstance(value[0], tuple):
        return "{" + ", ".join("%s=%s" % (k, fmt(v)) for k, v in value) + "}"
    if isinstance(value, list):
        return "[" + ", ".join(fmt(v) for v in value) + "]"
    return str(value)


def main():
    exports, names = load_tables(sys.argv[1])
    with open(sys.argv[2], "rb") as handle:
        data = handle.read()
    wanted = sys.argv[3] if len(sys.argv) > 3 else None
    reader = Reader(data, names, exports)
    for index in sorted(exports):
        export = exports[index]
        path = export["path"]
        if ".Main_Sequence." not in path or path.count(".") > 4:
            continue
        short = path.replace("TheWorld.PersistentLevel.", "")
        if export["size"] < 12:
            continue
        start = export["offset"] + 4  # skip NetIndex
        try:
            props, _ = reader.properties(start, export["offset"] + export["size"])
        except (struct.error, KeyError, UnicodeDecodeError) as error:
            props = [("<parse error>", str(error))]
        text = "\n".join("    %s = %s" % (k, fmt(v)) for k, v in props)
        if wanted and not re.search(wanted, short + "\n" + text):
            continue
        print("== %s" % short)
        print(text)


if __name__ == "__main__":
    main()
