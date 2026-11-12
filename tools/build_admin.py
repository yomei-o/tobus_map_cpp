# 東京都の輪郭（外周）と 23 区の区界を data/admin_tokyo.txt に取り出す。
#
#   python tools/build_admin.py <N03-YYYYMMDD_13.geojson> > data/admin_tokyo.txt
#
# 元: 国土数値情報「行政区域データ」N03（国土交通省）CC BY 4.0
#     https://nlftp.mlit.go.jp/ksj/gml/data/N03/  の東京都（コード 13）
# 出力（座標は lon,lat。各マップが自分の投影で km に直す）:
#   pref               続く ring が都の外周（本土のみ。伊豆・小笠原の島は除外）
#   ward <区名>        続く ring がその区の区界
#   ring lon,lat ...   多角形の輪（小数 5 桁、間引き済み）
# GeoJSON は UTF-8。標準ライブラリだけ。
import json, sys

# 本土だけ（伊豆・小笠原の島を外す）
BBOX = (138.9, 35.4, 140.05, 36.05)
TOL  = 0.0004      # 間引きの許容（度）。約 40 m
Q    = 1_000_000   # dissolve 用の座標量子化（1e-6 度 ≒ 0.1 m）


def simplify(pts, tol):
    if len(pts) < 4:
        return pts
    keep = [False] * len(pts); keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]; bx, by = pts[b]; dx, dy = bx - ax, by - ay
        L = (dx * dx + dy * dy) ** 0.5; best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / L if L > 0 else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > best:
                best, bi = d, i
        if best > tol:
            keep[bi] = True; stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]


def in_bbox(r):
    return any(BBOX[0] <= x <= BBOX[2] and BBOX[1] <= y <= BBOX[3] for x, y in r)


def dissolve(polys):
    # 辺が 2 回出たら内部（隣の区と共有）→ 消す。1 回だけの辺が外周。
    from collections import defaultdict
    cnt = defaultdict(int)
    def q(p): return (round(p[0] * Q), round(p[1] * Q))
    coord = {}
    for rings in polys:
        for r in rings:
            for i in range(len(r) - 1):
                a, b = q(r[i]), q(r[i + 1])
                if a == b:
                    continue
                coord[a] = r[i]; coord[b] = r[i + 1]
                cnt[(a, b) if a < b else (b, a)] += 1
    adj = defaultdict(list)
    for (a, b), c in cnt.items():
        if c == 1:
            adj[a].append(b); adj[b].append(a)
    used = set(); out = []
    for start in list(adj):
        for first in adj[start]:
            e = (start, first) if start < first else (first, start)
            if e in used:
                continue
            used.add(e); chain = [start, first]; prev, cur = start, first
            while cur != start:
                nxt = None
                for x in adj[cur]:
                    if x == prev:
                        continue
                    e2 = (cur, x) if cur < x else (x, cur)
                    if e2 not in used:
                        nxt = x; used.add(e2); break
                if nxt is None:
                    break
                prev, cur = cur, nxt; chain.append(cur)
            if len(chain) >= 4:
                out.append([coord[c] for c in chain])
    return out


def main(path):
    d = json.load(open(path, encoding="utf-8"))
    pref_polys = []
    wards = {}  # code -> [name, rings]
    for f in d["features"]:
        p = f["properties"]; code = str(p.get("N03_007") or "")
        geom = f["geometry"]
        if geom["type"] == "Polygon":
            rings = [[tuple(pt) for pt in geom["coordinates"][0]]] + \
                    [[tuple(pt) for pt in h] for h in geom["coordinates"][1:]]
        else:
            continue
        rings = [r for r in rings if in_bbox(r)]
        if not rings:
            continue
        if code != "13000":
            pref_polys.append(rings)
        if code.isdigit() and 13101 <= int(code) <= 13123:
            w = wards.setdefault(code, [p.get("N03_004") or code, []])
            w[1].extend(rings)

    out = ["# 出典: 国土数値情報「行政区域データ」N03（国土交通省）CC BY 4.0 を加工"
           "（東京都の外周と 23 区の区界、本土のみ）"]
    pref_rings = dissolve(pref_polys); pref_rings.sort(key=len, reverse=True)
    out.append("pref")
    for r in pref_rings:
        s = simplify(r, TOL)
        if len(s) >= 4:
            out.append("ring " + " ".join(f"{x:.5f},{y:.5f}" for x, y in s))
    for code in sorted(wards):
        name, rings = wards[code]
        out.append("ward " + name)
        for r in rings:
            s = simplify(r, TOL)
            if len(s) >= 4:
                out.append("ring " + " ".join(f"{x:.5f},{y:.5f}" for x, y in s))
    sys.stderr.write(f"pref rings={len(pref_rings)} wards={len(wards)}\n")
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
