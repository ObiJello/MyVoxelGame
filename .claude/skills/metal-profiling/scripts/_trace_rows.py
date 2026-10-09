#!/usr/bin/env python3
"""Per-encoder GPU time from an xctrace export (gpu_report.py's export dir):
pairs metal-gpu-intervals (execution per channel, keyed by encoder id) with
metal-object-label (encoder labels in creation order) by frame + order."""
import sys, collections, statistics
import xml.etree.ElementTree as ET

def rows(path):
    ids = {}
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag != "row": continue
        vals = []
        for c in el:
            if "ref" in c.attrib: vals.append(ids.get(c.attrib["ref"]))
            else:
                v = (c.text or "").strip(), c.attrib.get("fmt", ""), c
                if "id" in c.attrib: ids[c.attrib["id"]] = v
                # nested ids (formatted-label children)
                for sub in c.iter():
                    if "id" in sub.attrib and sub is not c:
                        ids[sub.attrib["id"]] = ((sub.text or "").strip(), sub.attrib.get("fmt", ""), sub)
                vals.append(v)
        yield vals
        el.clear()

def main(exp):
    # labels: frame -> [label of render/blit encoders in order]
    labels = collections.defaultdict(list)
    for v in rows(exp + "/metal-object-label.xml"):
        if not v[2] or not v[2][0].isdigit(): continue
        frame = int(v[2][0]); label = v[3][1]; typ = int(v[4][0])
        if typ == 11: labels[frame].append(label)
    # intervals: per frame, per encoder id: channel durations
    frames = collections.defaultdict(lambda: collections.OrderedDict())
    for v in rows(exp + "/metal-gpu-intervals.xml"):
        if v[0] is None: continue
        start = int(v[0][0]); dur = int(v[1][0]); chan = v[2][0]
        if chan not in ("Vertex", "Fragment"): continue
        if v[3] is None or not v[3][0].isdigit(): continue
        frame = int(v[3][0])
        fl = v[6][2] if v[6] else None
        enc = None
        if fl is not None:
            for sub in fl.iter():
                if sub.tag == "metal-encoder-id": enc = sub.text
        if v[6] and fl is None: enc = v[6][0]
        d = frames[frame].setdefault(enc, {"Vertex": 0, "Fragment": 0, "start": start})
        d[chan] += dur
        d["start"] = min(d["start"], start)
    per = collections.defaultdict(lambda: {"Vertex": [], "Fragment": [], "n": 0})
    unmatched = 0
    nframes = 0
    for frame, encs in frames.items():
        lab = [l for l in labels.get(frame, []) if not l.startswith("Blit")]
        ordered = sorted(encs.values(), key=lambda d: d["start"])
        if len(ordered) != len(lab):
            unmatched += 1
        nframes += 1
        for i, d in enumerate(ordered):
            name = lab[i] if i < len(lab) else "?"
            if name.startswith("Target"): name = "Target " + name.split("(")[1].rstrip(")") if "(" in name else name
            per[name]["Vertex"].append(d["Vertex"] / 1e6)
            per[name]["Fragment"].append(d["Fragment"] / 1e6)
            per[name]["n"] += 1
    print(f"frames {nframes}, frames whose encoder count != label count: {unmatched}")
    print(f"{'encoder':28s} {'per frame':>9s} {'vertex ms':>10s} {'frag ms':>9s} {'v p95':>7s} {'f p95':>7s}")
    for name, d in sorted(per.items(), key=lambda kv: -sum(kv[1]['Vertex']) - sum(kv[1]['Fragment'])):
        n = d["n"]
        vs, fs = sorted(d["Vertex"]), sorted(d["Fragment"])
        print(f"{name[:28]:28s} {n / nframes:9.2f} {sum(vs) / nframes:10.3f} {sum(fs) / nframes:9.3f} "
              f"{vs[int(len(vs) * .95)]:7.2f} {fs[int(len(fs) * .95)]:7.2f}")

if __name__ == "__main__":
    main(sys.argv[1])
