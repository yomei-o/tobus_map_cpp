// tobus_map.hpp — 都営バスのいま走っているバスを、光る地図に（系統の道・バス停・バス・一覧・吹き出しまで全部ここで描く）
//
// 使い方（ネイティブも WASM も同じ）:
//   tb::Map m; m.load(tobus_txt, font_bytes, n);
//   m.resize(1280, 800); m.fit();
//   m.set_buses(odpt_bus_json);          // ODPT の odpt:Bus（都営はキー無し）を受け取るたびに（約 30 秒ごと）
//   m.tick(dt, unix_seconds); const uint8_t* rgba = m.render();
//   m.hover(x, y); m.click(x, y); m.pan(dx, dy); m.zoom(f, x, y);
//
// バスの位置: ODPT が教えてくれるのは「どのバス停を何時に出て（fromBusstopPoleTime）、次はどのバス停か」。
// そのあいだを、系統の道の形に沿って、平均 4 m/s（約 14 km/h、信号待ちこみ）で進める。次のバス停に着いたと言われるまでは 95% の手前で待つ。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "raster.hpp"

namespace tb {

struct P2 { double x = 0, y = 0; };
inline P2 operator+(P2 a, P2 b) { return {a.x + b.x, a.y + b.y}; }
inline P2 operator-(P2 a, P2 b) { return {a.x - b.x, a.y - b.y}; }
inline P2 operator*(P2 a, double s) { return {a.x * s, a.y * s}; }

// 東京のあたりでは、経緯度を m にそのまま比例させてよい（正距円筒、基準は北緯 35.68 度）。y は北が負
inline P2 to_m(double lon, double lat) { return {(lon - 139.7) * 111320.0 * 0.81233, -(lat - 35.68) * 110574.0}; }

// "2026-11-12T09:55:24+09:00" → UNIX 秒
inline double parse_time(const std::string& s) {
    int Y, M, D, h, m, sec = 0, tzh = 9, tzm = 0; char sign = '+';
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &M, &D, &h, &m, &sec, &sign, &tzh, &tzm) < 6) return 0;
    int y = Y - (M <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400.0 + h * 3600.0 + m * 60.0 + sec - (sign == '-' ? -1 : 1) * (tzh * 3600.0 + tzm * 60.0);
}
inline std::string hhmmss(double t, bool sec = true) {
    long long s = (long long)std::floor(t + 9 * 3600) % 86400; if (s < 0) s += 86400;
    char b[16];
    if (sec) snprintf(b, sizeof b, "%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(b, sizeof b, "%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
    return b;
}

struct Pole { std::string id, name; P2 p; };
struct Pattern {
    std::string id, title;
    int route = -1;
    std::vector<P2> shape;
    std::vector<double> arc;        // 形の始めからの距離（m）
    std::vector<int> stops;         // バス停（poles の添字）
    std::vector<double> stop_arc;
    P2 lo{1e18, 1e18}, hi{-1e18, -1e18};
    // 形の上の距離 a の点と、その向き
    P2 at(double a, P2* dir = nullptr) const {
        if (shape.empty()) return {};
        a = std::clamp(a, 0.0, arc.back());
        size_t i = std::upper_bound(arc.begin(), arc.end(), a) - arc.begin();
        i = std::clamp<size_t>(i, 1, shape.size() - 1);
        double L = arc[i] - arc[i - 1], t = L > 0 ? (a - arc[i - 1]) / L : 0;
        if (dir) *dir = shape[i] - shape[i - 1];
        return shape[i - 1] + (shape[i] - shape[i - 1]) * t;
    }
};
struct Route { std::string name; uint32_t color = 0xffffff; std::vector<int> patterns; int buses = 0; };
struct Rail { uint32_t color; std::vector<P2> pts; };
struct Bus {
    std::string id, number;
    int pat = -1, k = -1;           // 系統パターンと、出たバス停の順番（stops の添字）。次は k + 1
    bool between = true;            // false = 終点など、次のバス停がわからない
    double t_from = 0;              // 出た時刻
    double disp = -1;               // 画面に出している位置（形の上の距離 m）
    bool fresh = false;
};

struct Button { float x0, y0, x1, y1; int id; };
enum { BTN_ALL = 1, BTN_FIT = 2, ROW0 = 100 };

// 系統名の頭の文字（「都」「品」「王」など）で色を分ける
inline uint32_t route_color(const std::string& name) {
    static const uint32_t PAL[] = {0x4FC3F7, 0xFF8A65, 0xAED581, 0xBA68C8, 0xFFD54F, 0x4DB6AC, 0xF06292, 0x7986CB,
                                   0xFFB74D, 0x81C784, 0x64B5F6, 0xE57373, 0x9575CD, 0xDCE775};
    std::string head;
    for (size_t i = 0; i < name.size();) {
        unsigned char c = name[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        std::string ch = name.substr(i, n);
        // 数字（全角・半角）が来たら終わり
        if ((c >= '0' && c <= '9') || (ch >= "０" && ch <= "９")) break;
        head += ch; i += n;
    }
    uint32_t h = 2166136261u;
    for (unsigned char c : head) h = (h ^ c) * 16777619u;
    return PAL[h % (sizeof PAL / sizeof PAL[0])];
}

struct Map {
    std::vector<Pole> poles;
    std::vector<Pattern> pats;
    std::vector<Route> routes;
    std::vector<Rail> rails;
    std::map<std::string, int> pole_of, pat_of;
    std::vector<Bus> buses;
    Font font;
    int W = 0, H = 0;
    double ui = 1, wall = 0;
    P2 center{0, 0};
    double scale = 0.05;              // 1 m あたりの px
    Canvas base, info, frame, glow;
    Mask mask;
    std::vector<uint8_t> rgba;
    bool dirty_base = true, dirty_info = true;
    int sel_route = -1;
    double bus_time = 0;              // 最後に受け取った時刻
    int fetch_state = 0;              // 0 = 読み込み中、1 = 取れた、2 = 取れなかった
    double hover_x = -1, hover_y = -1;
    std::vector<Button> buttons;
    struct Hub { std::string name; P2 p; int n; };
    std::vector<Hub> hubs;            // 系統がたくさん集まるバス停（名前を出す順）
    static constexpr double V = 4.0;  // バスの平均の速さ（m/s）

    // ---- 読み込み ----
    bool load(const std::string& text, const unsigned char* font_bytes, size_t font_n) {
        std::istringstream in(text);
        std::string ln;
        std::map<std::string, int> route_of;
        auto pts_of = [](const std::string& s, size_t from) {
            std::vector<P2> v;
            const char* p = s.c_str() + from; char* e;
            while (*p) {
                while (*p == ' ') ++p;
                double lon = strtod(p, &e); if (e == p) break; p = e; if (*p == ',') ++p;
                double lat = strtod(p, &e); if (e == p) break; p = e;
                v.push_back(to_m(lon, lat));
            }
            return v;
        };
        while (std::getline(in, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            std::istringstream s(ln);
            std::string kind; s >> kind;
            if (kind == "pole") {
                Pole p; double lon, lat; s >> p.id >> lon >> lat; std::getline(s >> std::ws, p.name);
                p.p = to_m(lon, lat);
                pole_of[p.id] = (int)poles.size(); poles.push_back(p);
            } else if (kind == "pattern") {
                Pattern P; std::string rname; s >> P.id >> rname; std::getline(s >> std::ws, P.title);
                auto it = route_of.find(rname);
                if (it == route_of.end()) { it = route_of.emplace(rname, (int)routes.size()).first; routes.push_back({rname, route_color(rname), {}, 0}); }
                P.route = it->second;
                routes[P.route].patterns.push_back((int)pats.size());
                pat_of[P.id] = (int)pats.size(); pats.push_back(P);
            } else if (kind == "shape" && !pats.empty()) {
                Pattern& P = pats.back();
                P.shape = pts_of(ln, 6);
                P.arc.assign(P.shape.size(), 0);
                for (size_t i = 1; i < P.shape.size(); ++i) { P2 d = P.shape[i] - P.shape[i - 1]; P.arc[i] = P.arc[i - 1] + std::sqrt(d.x * d.x + d.y * d.y); }
                for (auto& q : P.shape) { P.lo.x = std::min(P.lo.x, q.x); P.lo.y = std::min(P.lo.y, q.y); P.hi.x = std::max(P.hi.x, q.x); P.hi.y = std::max(P.hi.y, q.y); }
            } else if (kind == "stops" && !pats.empty()) {
                std::string tok;
                while (s >> tok) {
                    size_t at = tok.find('@');
                    auto it = pole_of.find(tok.substr(0, at));
                    if (it == pole_of.end() || at == std::string::npos) continue;
                    pats.back().stops.push_back(it->second);
                    pats.back().stop_arc.push_back(atof(tok.c_str() + at + 1));
                }
            } else if (kind == "rail") {
                std::string col; s >> col;
                rails.push_back({(uint32_t)strtoul(col.c_str(), nullptr, 16), pts_of(ln, 5 + col.size() + 1)});
            }
        }
        // 名前を出すバス停：同じ名前ののりばをまとめ、通る系統の数が多い順
        std::map<std::string, std::pair<P2, std::vector<int>>> agg;
        std::map<std::string, int> cnt;
        for (auto& P : pats) {
            std::vector<std::string> seen;
            for (int s : P.stops) {
                const std::string& nm = poles[s].name;
                if (std::find(seen.begin(), seen.end(), nm) != seen.end()) continue;
                seen.push_back(nm); ++cnt[nm];
                auto& a = agg[nm]; a.first = a.first + poles[s].p; a.second.push_back(s);
            }
        }
        for (auto& kv : agg) hubs.push_back({kv.first, kv.second.first * (1.0 / kv.second.second.size()), cnt[kv.first]});
        std::sort(hubs.begin(), hubs.end(), [](const Hub& a, const Hub& b) { return a.n > b.n; });
        if (font_bytes && font_n) font.load(font_bytes, font_n);
        return !pats.empty();
    }
    void resize(int w, int h) {
        W = w; H = h;
        base.resize(w, h); info.resize(w, h); frame.resize(w, h); glow.resize(w, h); mask.resize(w, h);
        rgba.assign((size_t)w * h * 4, 255);
        dirty_base = dirty_info = true;
    }
    bool side_panel() const { return W / ui >= 760; }
    // 23 区のあたりが入るように（青梅の系統は 40 km ほど離れているので、真ん中から 20 km より遠いバス停は除く）
    void fit() {
        std::vector<double> xs, ys;
        for (auto& p : poles) { xs.push_back(p.p.x); ys.push_back(p.p.y); }
        if (xs.empty()) return;
        std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
        P2 mid{xs[xs.size() / 2], ys[ys.size() / 2]}, lo{1e18, 1e18}, hi{-1e18, -1e18};
        for (auto& p : poles) {
            P2 d = p.p - mid;
            if (d.x * d.x + d.y * d.y > 20000.0 * 20000.0) continue;
            lo.x = std::min(lo.x, p.p.x); lo.y = std::min(lo.y, p.p.y); hi.x = std::max(hi.x, p.p.x); hi.y = std::max(hi.y, p.p.y);
        }
        double pad_r = side_panel() ? 300 * ui : 0;
        center = (lo + hi) * 0.5;
        scale = std::min((W - pad_r) / (hi.x - lo.x), (double)H / (hi.y - lo.y)) * 0.92;
        center.x += pad_r / 2 / scale;
        dirty_base = dirty_info = true;
    }
    void pan(double dx, double dy) { center.x -= dx / scale; center.y -= dy / scale; dirty_base = dirty_info = true; }
    void zoom(double f, double sx, double sy) {
        P2 w = to_world(sx, sy);
        scale = std::clamp(scale * f, 0.002, 2.0);
        P2 w2 = to_world(sx, sy);
        center = center + (w - w2);
        dirty_base = dirty_info = true;
    }
    P2 to_world(double sx, double sy) const { return {center.x + (sx - W / 2.0) / scale, center.y + (sy - H / 2.0) / scale}; }
    V2 to_screen(P2 p) const { return {(float)((p.x - center.x) * scale + W / 2.0), (float)((p.y - center.y) * scale + H / 2.0)}; }
    float px(float v) const { return v * (float)ui; }
    int fpx(float v) const { return std::max(8, (int)std::lround(v * ui)); }

    // ---- バス ----
    // ODPT の odpt:Bus（JSON の配列）。戻り値は地図に載ったバスの数
    int set_buses(const std::string& text) {
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_array()) { fetch_state = 2; dirty_info = true; return (int)buses.size(); }
        auto str = [](const nlohmann::json& o, const char* k) { auto it = o.find(k); return it != o.end() && it->is_string() ? it->get<std::string>() : std::string(); };
        std::map<std::string, Bus> old;
        for (auto& b : buses) old[b.id] = b;
        std::vector<Bus> next;
        for (auto& o : j) {
            if (!o.is_object()) continue;
            auto pt = pat_of.find(str(o, "odpt:busroutePattern"));
            if (pt == pat_of.end()) continue;
            const Pattern& P = pats[pt->second];
            auto fp = pole_of.find(str(o, "odpt:fromBusstopPole")), tp = pole_of.find(str(o, "odpt:toBusstopPole"));
            if (fp == pole_of.end()) continue;
            // 出たバス停の順番（同じバス停が 2 回出る系統もあるので、次のバス停と並んでいるところを優先）
            int k = -1;
            for (size_t i = 0; i < P.stops.size(); ++i) {
                if (P.stops[i] != fp->second) continue;
                if (tp != pole_of.end() && i + 1 < P.stops.size() && P.stops[i + 1] == tp->second) { k = (int)i; break; }
                if (k < 0) k = (int)i;
            }
            if (k < 0) continue;
            Bus b;
            std::string id = str(o, "owl:sameAs");
            auto it = old.find(id);
            if (it != old.end() && it->second.pat == pt->second) b = it->second;   // 前から見えているバス（表示の位置を引き継ぐ）
            b.id = id; b.number = str(o, "odpt:busNumber"); b.pat = pt->second; b.k = k;
            b.between = tp != pole_of.end() && k + 1 < (int)P.stops.size() && P.stops[k + 1] == tp->second;
            b.t_from = parse_time(str(o, "odpt:fromBusstopPoleTime"));
            if (b.t_from <= 0) b.t_from = wall;
            next.push_back(b);
        }
        buses.swap(next);
        for (auto& r : routes) r.buses = 0;
        for (auto& b : buses) routes[pats[b.pat].route].buses++;
        bus_time = wall; fetch_state = 1;
        dirty_info = true;
        return (int)buses.size();
    }
    void fetch_failed() { fetch_state = 2; dirty_info = true; }

    // いまいるはずの位置（形の上の距離 m）
    double target(const Bus& b) const {
        const Pattern& P = pats[b.pat];
        double a0 = P.stop_arc[b.k];
        if (!b.between) return a0;
        double a1 = P.stop_arc[b.k + 1], est = std::max(30.0, (a1 - a0) / V);
        return a0 + (a1 - a0) * std::clamp((wall - b.t_from) / est, 0.0, 0.95);
    }
    void tick(double dt, double now) {
        wall = now;
        for (auto& b : buses) {
            double t = target(b);
            if (b.disp < 0 || t < b.disp - 80 || t > b.disp + 3000) { b.disp = t; continue; }   // 初めて・大きく戻った・飛んだ
            if (t > b.disp) b.disp = std::min(t, b.disp + std::max(V * 3, (t - b.disp) * 0.8) * dt);   // 遅れていれば追いつく
        }
    }

    // ---- 描画 ----
    void draw_base() {
        Rgb top = lin({0.02f, 0.03f, 0.05f}), bot = lin({0.01f, 0.012f, 0.025f});
        for (int y = 0; y < H; ++y) {
            Rgb c = mix(top, bot, (float)y / std::max(1, H - 1));
            for (int x = 0; x < W; ++x) { float* p = &base.px[((size_t)y * W + x) * 3]; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
        }
        // 1 km の格子（近いときだけ）
        if (scale * 1000 > 40) {
            P2 lo = to_world(0, 0), hi = to_world(W, H);
            for (double gx = std::floor(lo.x / 1000) * 1000; gx <= hi.x; gx += 1000) polyline(base, {to_screen({gx, lo.y}), to_screen({gx, hi.y})}, 1, {0.2f, 0.25f, 0.35f}, 0.08f);
            for (double gy = std::floor(lo.y / 1000) * 1000; gy <= hi.y; gy += 1000) polyline(base, {to_screen({lo.x, gy}), to_screen({hi.x, gy})}, 1, {0.2f, 0.25f, 0.35f}, 0.08f);
        }
        P2 wlo = to_world(-40, -40), whi = to_world(W + 40, H + 40);
        auto visible = [&](P2 lo, P2 hi) { return !(hi.x < wlo.x || lo.x > whi.x || hi.y < wlo.y || lo.y > whi.y); };
        auto scr = [&](const std::vector<P2>& v) {
            std::vector<V2> out;
            for (auto& p : v) { V2 s = to_screen(p); if (!out.empty() && std::fabs(s.x - out.back().x) + std::fabs(s.y - out.back().y) < 0.7f) continue; out.push_back(s); }
            if (out.size() == 1) out.push_back(out[0]);
            return out;
        };
        // 地下鉄（目印、薄く）
        for (auto& r : rails) polyline(base, scr(r.pts), std::max(1.f, px(1.4f)), hex(r.color), 0.22f);
        // 系統の道：全部を 1 枚に集めて、光らせてから塗る
        const float lw = std::clamp((float)(scale * 9), px(0.8f), px(3.0f));
        mask.clear();
        for (auto& P : pats) if (visible(P.lo, P.hi)) mask_polyline(mask, scr(P.shape), lw);
        std::fill(glow.px.begin(), glow.px.end(), 0.f);
        const float dimk = sel_route >= 0 ? 0.3f : 1.f;   // 系統を選んでいるときは、ほかの道を暗く
        mask_paint(glow, mask, {0.2f, 0.55f, 0.9f}, 0.9f * dimk, true);
        bloom(base, glow, std::max(2, (int)px(4)), 0.9f);
        mask_paint(base, mask, {0.32f, 0.55f, 0.78f}, 0.75f * dimk);
        // 選んだ系統は、その色で明るく
        if (sel_route >= 0) {
            mask.clear();
            for (int pi : routes[sel_route].patterns) mask_polyline(mask, scr(pats[pi].shape), lw * 2.2f);
            std::fill(glow.px.begin(), glow.px.end(), 0.f);
            mask_paint(glow, mask, hex(routes[sel_route].color), 1.f, true);
            bloom(base, glow, std::max(2, (int)px(5)), 1.2f);
            mask_paint(base, mask, hex(routes[sel_route].color), 1.f);
        }
        // バス停（近いときだけ点）
        if (scale > 0.06) {
            float r = std::clamp((float)(scale * 12), px(1.2f), px(3.0f));
            for (auto& p : poles) {
                V2 s = to_screen(p.p);
                if (s.x < -5 || s.y < -5 || s.x > W + 5 || s.y > H + 5) continue;
                disc(base, s, r + 1, {0.02f, 0.03f, 0.05f}, 0.8f);
                disc(base, s, r, {0.85f, 0.9f, 1.f}, 0.85f);
            }
        }
        draw_hub_labels();
    }
    // バス停の名前：系統が多く集まる順に、重ならないところへ。拡大するほど多く出す
    void draw_hub_labels() {
        if (!font.ok) return;
        struct Box { float x0, y0, x1, y1; };
        std::vector<Box> used;
        int limit = (int)std::clamp(scale * 1200, 12.0, 250.0);
        int shown = 0;
        int sz = fpx(scale > 0.08 ? 12.5f : 11.5f);
        for (auto& h : hubs) {
            if (shown >= limit) break;
            V2 p = to_screen(h.p);
            if (p.x < 0 || p.y < 0 || p.x > W || p.y > H) continue;
            float w = font.width(h.name, sz), th = sz * 0.9f;
            V2 cand[4] = {{p.x + px(6), p.y + th * 0.38f}, {p.x - px(6) - w, p.y + th * 0.38f}, {p.x - w / 2, p.y - px(6)}, {p.x - w / 2, p.y + px(6) + th}};
            for (auto c : cand) {
                Box b{c.x - 2, c.y - th - 1, c.x + w + 2, c.y + 3};
                if (b.x0 < 0 || b.y0 < 0 || b.x1 > W || b.y1 > H) continue;
                bool hit = false;
                for (auto& u : used) if (!(b.x1 < u.x0 || b.x0 > u.x1 || b.y1 < u.y0 || b.y0 > u.y1)) { hit = true; break; }
                if (hit) continue;
                disc(base, p, px(2.2f), {1, 1, 1}, 0.9f);
                font.draw(base, c.x, c.y, h.name, sz, {0.85f, 0.9f, 0.96f}, {0.01f, 0.015f, 0.03f}, 2.f);
                used.push_back(b); ++shown;
                break;
            }
        }
    }

    void text(Canvas& cv, float x, float y, const std::string& s, float size, Rgb c) { font.draw(cv, x, y, s, fpx(size), c, {0, 0, 0}, 0); }
    float tw(const std::string& s, float size) { return font.width(s, fpx(size)); }
    void panel(Canvas& cv, float x0, float y0, float x1, float y1) { round_rect(cv, x0, y0, x1, y1, px(12), {0.055f, 0.075f, 0.115f}, 0.86f); }
    void pill(Canvas& cv, float x, float y, float h, const std::string& s, uint32_t col) {
        float w = tw(s, h / (float)ui * 0.55f) + px(14);
        round_rect(cv, x, y, x + w, y + h, h / 2, hex(col));
        text(cv, x + px(7), y + h * 0.5f + px(h / (float)ui * 0.55f) * 0.36f, s, h / (float)ui * 0.55f, {0.03f, 0.05f, 0.08f});
    }
    void button(Canvas& cv, float x, float y, const std::string& s, int id, bool on = false) {
        float w = tw(s, 13) + px(22), h = px(30);
        round_rect(cv, x, y, x + w, y + h, px(8), on ? Rgb{0.35f, 0.69f, 1.0f} : Rgb{0.16f, 0.2f, 0.28f}, 0.92f);
        text(cv, x + px(11), y + h / 2 + px(13) * 0.36f, s, 13, on ? Rgb{0.02f, 0.07f, 0.12f} : Rgb{0.9f, 0.93f, 0.97f});
        buttons.push_back({x, y, x + w, y + h, id});
    }
    std::vector<int> route_order() const {
        std::vector<int> o;
        for (size_t i = 0; i < routes.size(); ++i) if (routes[i].buses > 0) o.push_back((int)i);
        std::stable_sort(o.begin(), o.end(), [&](int a, int b) { return routes[a].buses > routes[b].buses; });
        return o;
    }
    void draw_panels(Canvas& cv) {
        buttons.clear();
        const Rgb fg{0.9f, 0.93f, 0.97f}, muted{0.55f, 0.6f, 0.68f};
        float x0 = px(16), y0 = px(16), wpan = std::min((float)W - px(32), px(340));
        float hpan = px(sel_route >= 0 ? 150 : 112);
        panel(cv, x0, y0, x0 + wpan, y0 + hpan);
        float y = y0 + px(30);
        text(cv, x0 + px(14), y, "都営バス いま走っているバス", 17, fg);
        y += px(22);
        std::string st = fetch_state == 0 ? "読み込み中…" : std::to_string(buses.size()) + " 台（" + hhmmss(bus_time) + " 更新）";
        if (fetch_state == 2) st += "・取れませんでした";
        text(cv, x0 + px(14), y, st, 12, muted);
        y += px(12);
        if (sel_route >= 0) {
            const Route& r = routes[sel_route];
            y += px(6);
            pill(cv, x0 + px(14), y, px(26), r.name, r.color);
            text(cv, x0 + px(14) + tw(r.name, 26 * 0.55f) + px(24), y + px(18), std::to_string(r.buses) + " 台が走っています", 13, fg);
            y += px(34);
        } else {
            text(cv, x0 + px(14), y + px(14), "系統を選ぶと、その道とバスを強調します", 11.5f, muted);
            y += px(24);
        }
        button(cv, x0 + px(14), y, "すべての系統", BTN_ALL, sel_route < 0);
        button(cv, buttons.back().x1 + px(8), y, "全体", BTN_FIT);
        // 系統の一覧（走っている台数の多い順）
        auto order = route_order();
        float lx0, ly0, lx1, ly1, rowh = px(30);
        if (side_panel()) { lx1 = W - px(16); lx0 = lx1 - px(270); ly0 = px(16); ly1 = H - px(40); }
        else {
            lx0 = px(16); lx1 = W - px(16); ly1 = H - px(40);
            int rows = std::clamp((int)((ly1 - (y0 + hpan + px(12)) - px(42)) / rowh), 1, 5);
            ly0 = ly1 - px(42) - rows * rowh;
        }
        if (!order.empty() && ly1 - ly0 > rowh + px(40)) {
            panel(cv, lx0, ly0, lx1, ly1);
            text(cv, lx0 + px(14), ly0 + px(24), "系統（走っている台数の多い順）", 12.5f, fg);
            float ry = ly0 + px(36);
            int maxb = routes[order[0]].buses;
            int cols = side_panel() ? 1 : 2;
            float cw = (lx1 - lx0 - px(12)) / cols;
            for (size_t i = 0; i < order.size(); ++i) {
                int c = (int)i % cols;
                if (c == 0 && i > 0) ry += rowh;
                if (ry + rowh > ly1 - px(6)) break;
                const Route& r = routes[order[i]];
                float cx = lx0 + px(6) + c * cw;
                if (order[i] == sel_route) round_rect(cv, cx, ry, cx + cw, ry + rowh - px(2), px(7), {0.2f, 0.32f, 0.5f}, 0.6f);
                pill(cv, cx + px(6), ry + px(4), px(20), r.name, r.color);
                float bx = std::max(cx + px(76), cx + px(6) + tw(r.name, 20 * 0.55f) + px(22));
                float bw = std::max(0.f, cx + cw - px(34) - bx) * r.buses / std::max(1, maxb);
                round_rect(cv, bx, ry + px(11), bx + std::max(px(2), bw), ry + px(18), px(3), hex(r.color), 0.7f);
                text(cv, cx + cw - px(30), ry + px(19), std::to_string(r.buses), 12, fg);
                buttons.push_back({cx, ry, cx + cw, ry + rowh, ROW0 + order[i]});
            }
        }
        std::string src = "バスの位置・系統・停留所：ODPT（東京都交通局）　地下鉄の線路：国土数値情報　文字：Noto Sans JP";
        text(cv, std::max(px(16), W - px(16) - tw(src, 10)), H - px(16), src, 10, {0.45f, 0.5f, 0.58f});
    }

    // バス 1 台を描く（尾は来た道に沿って）
    void draw_bus(Canvas& cv, const Bus& b, float lw, bool dim) {
        const Pattern& P = pats[b.pat];
        V2 head = to_screen(P.at(b.disp));
        if (head.x < -40 || head.y < -40 || head.x > W + 40 || head.y > H + 40) return;
        Rgb c = hex(routes[P.route].color);
        float a = dim ? 0.25f : 1.f;
        double tail_m = lw * 14 / scale;
        const int n = 10;
        std::vector<V2> tail;
        for (int k = 0; k <= n; ++k) tail.push_back(to_screen(P.at(b.disp - tail_m * k / n)));
        for (int k = n; k >= 1; --k) {
            float f = 1.f - (float)k / (n + 1);
            polyline(cv, {tail[k], tail[k - 1]}, lw * (0.4f + 1.4f * f), mix(c, {1, 1, 1}, 0.35f * f), 0.85f * f * f * a, true);
        }
        glow_dot(cv, head, lw * 4.5f, c, 0.9f * a);
        disc(cv, head, lw * 1.2f, mix(c, {1, 1, 1}, 0.7f), a);
    }
    const Bus* pick_bus(double x, double y) const {
        const Bus* best = nullptr; double bd = px(12);
        for (auto& b : buses) {
            if (sel_route >= 0 && pats[b.pat].route != sel_route) continue;
            V2 p = to_screen(pats[b.pat].at(b.disp));
            double d = std::hypot(p.x - x, p.y - y);
            if (d < bd) { bd = d; best = &b; }
        }
        return best;
    }
    const Pole* pick_pole(double x, double y) const {
        if (scale <= 0.06) return nullptr;
        const Pole* best = nullptr; double bd = px(8);
        for (auto& p : poles) { V2 s = to_screen(p.p); double d = std::hypot(s.x - x, s.y - y); if (d < bd) { bd = d; best = &p; } }
        return best;
    }
    void draw_tip(Canvas& cv) {
        if (hover_x < 0) return;
        std::vector<std::pair<std::string, Rgb>> lines;
        if (const Bus* b = pick_bus(hover_x, hover_y)) {
            const Pattern& P = pats[b->pat];
            lines.push_back({P.title, hex(routes[P.route].color)});
            if (b->between) lines.push_back({"次は " + poles[P.stops[b->k + 1]].name, {1, 1, 1}});
            lines.push_back({poles[P.stops[b->k]].name + " を " + hhmmss(b->t_from, false) + " に出発", {0.75f, 0.8f, 0.88f}});
            if (!b->number.empty()) lines.push_back({"車両 " + b->number, {0.6f, 0.65f, 0.72f}});
        } else if (const Pole* p = pick_pole(hover_x, hover_y)) {
            lines.push_back({p->name, {1, 1, 1}});
            std::vector<std::string> rs;
            int idx = (int)(p - &poles[0]);
            for (auto& P : pats) if (std::find(P.stops.begin(), P.stops.end(), idx) != P.stops.end() && std::find(rs.begin(), rs.end(), routes[P.route].name) == rs.end()) rs.push_back(routes[P.route].name);
            std::string s;
            for (size_t i = 0; i < rs.size() && i < 8; ++i) s += (i ? "・" : "") + rs[i];
            if (rs.size() > 8) s += " ほか";
            if (!s.empty()) lines.push_back({s, {0.75f, 0.8f, 0.88f}});
        }
        if (lines.empty()) return;
        float w = 0;
        for (auto& l : lines) w = std::max(w, tw(l.first, 12.5f));
        w += px(20);
        float h = px(10 + 19 * lines.size());
        float x = std::min((float)hover_x + px(14), W - w - px(4)), y = std::min((float)hover_y + px(14), H - h - px(4));
        round_rect(cv, x, y, x + w, y + h, px(8), {0.06f, 0.08f, 0.12f}, 0.93f);
        for (size_t i = 0; i < lines.size(); ++i) text(cv, x + px(10), y + px(22 + 19 * i), lines[i].first, 12.5f, lines[i].second);
    }

    const uint8_t* render() {
        if (dirty_base) { draw_base(); dirty_base = false; dirty_info = true; }
        if (dirty_info) { info.px = base.px; draw_panels(info); dirty_info = false; }
        frame.px = info.px;
        const float lw = std::clamp((float)(scale * 14), px(1.4f), px(4.0f));
        for (auto& b : buses) if (sel_route >= 0 && pats[b.pat].route != sel_route) draw_bus(frame, b, lw, true);
        for (auto& b : buses) if (sel_route < 0 || pats[b.pat].route == sel_route) draw_bus(frame, b, lw * (sel_route >= 0 ? 1.3f : 1.f), false);
        draw_tip(frame);
        frame.to_rgba(rgba.data());
        return rgba.data();
    }

    // ---- 操作 ----
    void hover(double x, double y) { hover_x = x; hover_y = y; }
    int hit(double x, double y) const {
        for (auto& b : buttons) if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return b.id;
        return 0;
    }
    void select_route(int r) {
        sel_route = r;
        dirty_base = dirty_info = true;
        if (r < 0) return;
        // その系統が全部入るように寄せる
        P2 lo{1e18, 1e18}, hi{-1e18, -1e18};
        for (int pi : routes[r].patterns) { auto& P = pats[pi]; lo.x = std::min(lo.x, P.lo.x); lo.y = std::min(lo.y, P.lo.y); hi.x = std::max(hi.x, P.hi.x); hi.y = std::max(hi.y, P.hi.y); }
        double pad_r = side_panel() ? 300 * ui : 0;
        center = (lo + hi) * 0.5;
        scale = std::clamp(std::min((W - pad_r) / std::max(1.0, hi.x - lo.x), (H * 0.7) / std::max(1.0, hi.y - lo.y)) * 0.8, 0.002, 0.3);
        center.x += pad_r / 2 / scale;
    }
    int click(double x, double y) {
        int id = hit(x, y);
        if (id == BTN_ALL) select_route(-1);
        else if (id == BTN_FIT) fit();
        else if (id >= ROW0) select_route(id - ROW0 == sel_route ? -1 : id - ROW0);
        return id != 0;
    }
};

}  // namespace tb
