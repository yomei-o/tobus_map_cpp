# 都営バスの系統（道路に沿った形とバス停の並び）とバス停を、ODPT のデータから data/tobus.txt に取り出す。
#
#   python tools/build_tobus.py <dir> [metro_network.txt] > data/tobus.txt
#
# <dir> には ODPT の公開用 API（キー無し）から取った JSON を置く:
#   BusstopPole.json      https://api-public.odpt.org/api/v4/odpt:BusstopPole?odpt:operator=odpt.Operator:Toei
#   BusroutePattern.json  https://api-public.odpt.org/api/v4/odpt:BusroutePattern?odpt:operator=odpt.Operator:Toei
# metro_network.txt（任意）: metro_map_cpp の data/network.txt。地下鉄の線路を目印として薄く描くのに使う
#
# 出力:
#   pole <ODPT のバス停（のりば）> <経度> <緯度> <名前>
#   pattern <ODPT の系統パターン> <系統名> <題名>     続く shape と stops がこのパターン
#   shape <経度>,<緯度> ...                         道路に沿った形（約 3 m で間引き）
#   stops <バス停>@<形の始めからの距離 m> ...       止まる順
#   rail <RRGGBB> <経度>,<緯度> ...                 地下鉄の線路（目印）
import json, math, os, sys

TOL = 3.0   # 間引きの許容（m）


def to_m(lon, lat, lat0=35.68):
    return lon * 111320.0 * math.cos(math.radians(lat0)), lat * 110574.0


def simplify(pts, tol):
    if len(pts) < 3:
        return pts
    m = [to_m(*p) for p in pts]
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        (ax, ay), (bx, by) = m[a], m[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy)
        best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = m[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / L if L > 0 else math.hypot(px - ax, py - ay)
            if d > best:
                best, bi = d, i
        if best > tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]


def arcs(pts):
    out, acc = [0.0], 0.0
    for (x0, y0), (x1, y1) in zip(map(lambda p: to_m(*p), pts), map(lambda p: to_m(*p), pts[1:])):
        acc += math.hypot(x1 - x0, y1 - y0)
        out.append(acc)
    return out


def project(pts, ac, p, start_arc):
    """点 p に一番近い形の上の位置（形の始めからの距離 m）。start_arc より手前（少しの戻りは許す）は見ない"""
    # 周回する系統は同じ場所を 2 回通るので、いちばん近いところではなく「前から見て最初に近づいたところ」を取る
    px, py = to_m(*p)
    best, best_arc = 1e18, start_arc
    near, near_arc = 1e18, None
    for i in range(len(pts) - 1):
        if ac[i + 1] < start_arc - 30:
            continue
        (ax, ay), (bx, by) = to_m(*pts[i]), to_m(*pts[i + 1])
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
        d = math.hypot(ax + dx * t - px, ay + dy * t - py)
        a = ac[i] + (ac[i + 1] - ac[i]) * t
        if d < best - 1e-6:
            best, best_arc = d, a
        if d < 60 and d < near:
            near, near_arc = d, a
        elif near_arc is not None and d > near + 120:
            break   # 近づいたあと離れた
    if near_arc is not None:
        return max(near_arc, start_arc), near
    return max(best_arc, start_arc), best


def main(d, metro=None):
    poles = {p["owl:sameAs"]: p for p in json.load(open(os.path.join(d, "BusstopPole.json"), encoding="utf-8"))}
    pats = json.load(open(os.path.join(d, "BusroutePattern.json"), encoding="utf-8"))
    out = ["# 出典: 公共交通オープンデータセンター（ODPT）を通じて東京都交通局が提供するデータを加工して作成"]
    used = set()
    body = []
    far = 0
    for pt in sorted(pats, key=lambda x: x["owl:sameAs"]):
        reg = pt.get("ug:region")
        if not reg or reg.get("type") != "LineString":
            continue
        shape = simplify([tuple(c[:2]) for c in reg["coordinates"]], TOL)
        ac = arcs(shape)
        title = (pt.get("dc:title") or "").strip()
        route = title.split()[0] if title else pt["odpt:busroute"].split(".")[-1]
        stops, arc = [], 0.0
        for o in sorted(pt.get("odpt:busstopPoleOrder", []), key=lambda o: o["odpt:index"]):
            pid = o["odpt:busstopPole"]
            p = poles.get(pid)
            if not p or p.get("geo:long") is None:
                continue
            arc, dist = project(shape, ac, (p["geo:long"], p["geo:lat"]), arc)
            if dist > 150:
                far += 1
            stops.append(f"{pid}@{arc:.0f}")
            used.add(pid)
        body.append(f"pattern {pt['owl:sameAs']} {route} {title}")
        body.append("shape " + " ".join(f"{x:.5f},{y:.5f}" for x, y in shape))
        body.append("stops " + " ".join(stops))
    for pid in sorted(used):
        p = poles[pid]
        name = (p.get("dc:title") or "").split("(")[0].strip()   # 「稲荷神社前(いなりじんじゃまえ…)」の読みを外す
        out.append(f"pole {pid} {p['geo:long']:.6f} {p['geo:lat']:.6f} {name}")
    out += body
    if metro:
        color = "FFFFFF"
        for l in open(metro, encoding="utf-8"):
            t = l.split()
            if t and t[0] == "line":
                color = t[2]
            elif t and t[0] == "track":
                pts = [tuple(map(float, s.split(","))) for s in t[1:]]
                pts = simplify(pts, 15)
                out.append(f"rail {color} " + " ".join(f"{x:.5f},{y:.5f}" for x, y in pts))
    print(f"patterns {sum(1 for b in body if b.startswith('pattern'))}, poles {len(used)}, stops far from shape {far}", file=sys.stderr)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
