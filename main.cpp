// tobus_map — ネイティブで都営バスの地図を PNG に描く（WASM と同じ tobus_map.hpp）
//   tobus_map <data/tobus.txt> <fonts/NotoSansJP-Bold-subset.ttf> <out_prefix> <W> <H> <bus1.json> [bus2.json ...]
//   バスの JSON（odpt:Bus）を 30 秒おきに届いたものとして順に渡す → <out>_all.png, <out>_zoom.png, <out>_route.png
#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "tobus_map.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image_write.h"

static std::string read_all(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: tobus_map data font out W H bus.json ...\n"); return 1; }
    std::string out = argv[3];
    int W = atoi(argv[4]), H = atoi(argv[5]);
    std::string data = read_all(argv[1]), font = read_all(argv[2]);
    tb::Map m;
    auto t0 = std::chrono::steady_clock::now();
    if (!m.load(data, (const unsigned char*)font.data(), font.size())) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    { std::string a = read_all("data/admin_tokyo.txt"); if (!a.empty()) m.load_admin(a); }   // 東京都の外周と 23 区
    printf("poles %zu, patterns %zu, routes %zu, rails %zu (load %.0f ms)\n", m.poles.size(), m.pats.size(), m.routes.size(), m.rails.size(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    m.resize(W, H); m.fit();
    for (int a = 6; a < argc; ++a) {
        std::string js = read_all(argv[a]);
        // 時計は、その JSON の中のいちばん新しい出発時刻の 20 秒あと
        double latest = 0;
        for (size_t p = js.find("fromBusstopPoleTime"); p != std::string::npos; p = js.find("fromBusstopPoleTime", p + 1))
            latest = std::max(latest, tb::parse_time(js.substr(js.find('"', p + 21) + 1, 25)));
        if (a == 6) m.wall = latest + 20; else m.wall += 30;
        int n = m.set_buses(js);
        int between = 0;
        for (auto& b : m.buses) between += b.between;
        printf("%s: %d buses (%d between stops)\n", argv[a], n, between);
        for (int k = 0; k < (a + 1 < argc ? 60 : 10); ++k) m.tick(0.5, m.wall += 0.5);
    }
    auto shot = [&](const char* name) {
        auto a = std::chrono::steady_clock::now();
        const uint8_t* px = m.render();
        auto b = std::chrono::steady_clock::now();
        m.render();
        auto c = std::chrono::steady_clock::now();
        std::string f = out + "_" + name + ".png";
        stbi_write_png(f.c_str(), W, H, 4, px, W * 4);
        printf("%s: first %.0f ms, next %.1f ms -> %s\n", name, std::chrono::duration<double, std::milli>(b - a).count(), std::chrono::duration<double, std::milli>(c - b).count(), f.c_str());
    };
    shot("all");
    m.zoom(6, W * 0.45, H * 0.5);
    shot("zoom");
    m.fit();
    auto order = m.route_order();
    if (!order.empty()) { m.select_route(order[0]); shot("route"); }
    return 0;
}
