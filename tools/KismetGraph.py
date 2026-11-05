"""Walk a Kismet graph dumped by DumpKismet.py.

usage: py -3 KismetGraph.py <map.kismet.txt> back|fwd|refs <OpName> [depth]

back  - ops that activate <OpName>, recursively
fwd   - ops activated by <OpName>, recursively
refs  - ops whose properties mention <OpName> (variables, event toggles)
"""
import re
import sys


def load(path):
    text = open(path, encoding="utf-8").read()
    ops = {}
    for block in re.split(r"^== ", text, flags=re.M)[1:]:
        name, _, body = block.partition("\n")
        ops[name.strip().replace("Main_Sequence.", "")] = body
    edges = []
    for name, body in ops.items():
        match = re.search(r"^    OutputLinks = (.*)$", body, re.M)
        if not match:
            continue
        for out in re.finditer(r"\{Links=\[(.*?)\], LinkDesc=([^,]*),.*?ActivateDelay=([0-9.]+)",
                               match.group(1)):
            for target in re.finditer(r"LinkedOp=([^,]+), InputLinkIdx=(\d+)", out.group(1)):
                edges.append((name, out.group(2), target.group(1).replace("Main_Sequence.", ""),
                              int(target.group(2)), out.group(3)))
    return ops, edges


def inputs(ops, op):
    match = re.search(r"^    InputLinks = (.*)$", ops.get(op, ""), re.M)
    return re.findall(r"LinkDesc=([^,]*)", match.group(1)) if match else []


def variables(ops, op):
    match = re.search(r"^    VariableLinks = (.*)$", ops.get(op, ""), re.M)
    if not match:
        return ""
    pairs = re.findall(r"LinkedVariables=\[([^\]]*)\], LinkDesc=([^,]*)", match.group(1))
    return "; ".join("%s=%s" % (desc, value.replace("Main_Sequence.", ""))
                     for value, desc in pairs if value)


def scalars(ops, op):
    keep = []
    for line in ops.get(op, "").splitlines():
        key = line.strip().split(" = ")[0]
        if key in ("InputLinks", "OutputLinks", "VariableLinks", "ObjPosX", "ObjPosY",
                   "DrawWidth", "DrawHeight", "ObjInstanceVersion", "ParentSequence", "MaxWidth"):
            continue
        keep.append(line.strip())
    return ", ".join(keep)


def describe(ops, op):
    extra = scalars(ops, op)
    var = variables(ops, op)
    return "%s%s%s" % (op, (" {" + extra + "}") if extra else "", (" vars: " + var) if var else "")


def walk(ops, edges, op, forward, depth, limit, seen):
    for source, output, target, index, delay in edges:
        node = source if forward else target
        if node != op or (source, output, target, index) in seen:
            continue
        seen.add((source, output, target, index))
        names = inputs(ops, target)
        label = names[index] if index < len(names) else str(index)
        other = target if forward else source
        print("%s%s [%s] --> %s.%s%s   | %s" % (
            "  " * depth, source, output, target, label,
            "" if delay in ("0.0", "0") else " delay=" + delay, describe(ops, other)))
        if depth < limit:
            walk(ops, edges, other, forward, depth + 1, limit, seen)


def main():
    ops, edges = load(sys.argv[1])
    mode, op = sys.argv[2], sys.argv[3]
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    if mode == "refs":
        for name, body in ops.items():
            if name != op and re.search(r"\b%s\b" % re.escape(op), body):
                print(describe(ops, name))
        return
    print(describe(ops, op))
    walk(ops, edges, op, mode == "fwd", 0, limit, set())


if __name__ == "__main__":
    main()
