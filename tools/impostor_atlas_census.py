#!/usr/bin/env python3
"""Which objects bind a backdrop-impostor atlas?  (evidence for n2_impostor_atlas)

Walks every 0x80134010 object in the shipped STREAM*.BUN bundles, reads its
0x134012 texture slots, resolves each key against the bundle's own TPK record
names, and lists the objects that touch one of the LOD/panorama atlases.

Result over all eight bundles (~26.9k objects): every such object is already a
backdrop by name (PAN_* / TRN_PANARAMA*) EXCEPT three leftovers that carry no
usable 0x134011 name -- OBJECT01 and two whose name leaf is not ASCII. Those
three reached the ordinary world scene, which is how TRN_COASTROADLOD_A_DM came
to lie across the conservatory park. Hence the classifier keys on the material.

    python3 tools/impostor_atlas_census.py /path/to/NFSU2/TRACKS
"""
import collections, glob, os, re, struct, sys

ATLAS = re.compile(r'^(TRN_(COASTROAD|FREEWAY)LOD_|TRN_TREELINE'
                   r'|TRN_TREES_FENCES_LOD|ARC_PANARAMABUILDINGS)')


def scan(path):
    d = open(path, 'rb').read()
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]

    # TPK records: 24-byte ASCII name at +0x00, texture key at +0x18.
    keymap = {}
    for m in re.finditer(rb'[A-Z][A-Za-z0-9_]{3,23}\x00', d):
        i = m.start()
        if i + 0x40 >= len(d):
            continue
        k = u32(i + 0x18)
        if k:
            keymap.setdefault(k, set()).add(m.group(0)[:-1].decode('ascii'))

    def leaves(beg, end, want, out):
        o = beg
        while o + 8 <= end:
            mg, sz = u32(o), u32(o + 4)
            ds = o + 8
            if ds + sz > end:
                return
            if mg == want:
                out.append((ds, sz))
            elif mg and (mg >> 28) == 8:
                leaves(ds, ds + sz, want, out)
            o = ds + sz

    def name(beg, end):
        out = []
        leaves(beg, end, 0x00134011, out)
        for off, sz in out:
            m = re.search(rb'[A-Za-z][A-Za-z0-9_]{4,}', d[off:off + sz])
            if m:
                return m.group(0).decode()
        return '(unnamed)'

    found, total = collections.Counter(), 0

    def walk(beg, end):
        nonlocal total
        o = beg
        while o + 8 <= end:
            mg, sz = u32(o), u32(o + 4)
            ds = o + 8
            if ds + sz > end:
                return
            if mg == 0x80134010:
                total += 1
                sl = []
                leaves(ds, ds + sz, 0x00134012, sl)
                names = [t for off, s in sl for b in range(0, s - 3, 8)
                         for t in keymap.get(u32(off + b), ())]
                if any(ATLAS.search(t) for t in names):
                    found[name(ds, ds + sz)] += 1
            elif mg and (mg >> 28) == 8:
                walk(ds, ds + sz)
            o = ds + sz

    walk(0, len(d))
    return total, found


def main(root):
    bundles = sorted(glob.glob(os.path.join(root, 'STREAM*.BUN')))
    if not bundles:
        sys.exit('no STREAM*.BUN under %s' % root)
    grand, objects, leaks = collections.Counter(), 0, 0
    for p in bundles:
        total, found = scan(p)
        objects += total
        print('%-16s %6d objects, %3d bind an impostor atlas'
              % (os.path.basename(p), total, sum(found.values())))
        for k, v in sorted(found.items()):
            named = k.startswith('PAN_') or k.startswith('TRN_PANARAMA')
            if not named:
                leaks += v
            print('     %4d  %-30s %s' % (v, k, '' if named else '<-- NOT named as backdrop'))
        grand.update(found)
    print('\n%d objects scanned, %d bind an atlas, %d of those are not named as backdrop'
          % (objects, sum(grand.values()), leaks))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'TRACKS')
