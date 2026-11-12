// WASM の入口。ページは JSON（ODPT の odpt:Bus）を取ってきて渡すだけで、画面はすべてここ（tobus_map.hpp）で描く。
//   build: sh wasm/build.sh → docs/tb.js, docs/tb.wasm
#include "tobus_map.hpp"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <emscripten/emscripten.h>

static tb::Map g;

extern "C" {
EMSCRIPTEN_KEEPALIVE int tb_init(const char* data, const unsigned char* font, int font_n) { return g.load(data, font, (size_t)font_n) ? (int)g.pats.size() : 0; }
EMSCRIPTEN_KEEPALIVE void tb_resize(int w, int h, double ui) { g.ui = ui; g.resize(w, h); }
EMSCRIPTEN_KEEPALIVE void tb_fit() { g.fit(); }
EMSCRIPTEN_KEEPALIVE void tb_pan(double dx, double dy) { g.pan(dx, dy); }
EMSCRIPTEN_KEEPALIVE void tb_zoom(double f, double x, double y) { g.zoom(f, x, y); }
EMSCRIPTEN_KEEPALIVE void tb_tick(double dt, double now) { g.tick(dt, now); }
EMSCRIPTEN_KEEPALIVE const uint8_t* tb_render() { return g.render(); }
// ODPT の odpt:Bus。戻り値は地図に載ったバスの数。取れなかったときは tb_fail
EMSCRIPTEN_KEEPALIVE int tb_buses(const char* json) { return g.set_buses(json); }
// 東京都の外周と 23 区の区界（data/admin_tokyo.txt）。戻り値は読み込んだ輪の数。目印として薄く描く
EMSCRIPTEN_KEEPALIVE int tb_admin(const char* text) { return g.load_admin(text); }
EMSCRIPTEN_KEEPALIVE void tb_fail() { g.fetch_failed(); }
// 操作。tb_hit はその点にボタンや一覧があるか（あればドラッグを始めない）
EMSCRIPTEN_KEEPALIVE void tb_hover(double x, double y) { g.hover(x, y); }
EMSCRIPTEN_KEEPALIVE int tb_hit(double x, double y) { return g.hit(x, y); }
EMSCRIPTEN_KEEPALIVE int tb_click(double x, double y) { return g.click(x, y); }
}
