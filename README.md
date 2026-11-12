# tobus_map_cpp — 都営バスのいま走っているバスを、光る地図に（C++ / WASM）

都営バス 150 系統の道とバス停を暗い地図に光らせ、いま走っているバス（昼間で 600 台ほど）を流れ星のような尾つきで走らせます。
画面はすべて C++ で描いています（道・バス停・バス・系統の一覧・ボタン・吹き出し・文字まで）。
ブラウザの JavaScript がやるのは、ODPT の JSON を 30 秒ごとに取って WASM に渡すことと、指・マウスの動きを渡すことだけです。

**デモ**: https://yomei-o.github.io/tobus_map_cpp/

- 右（スマホでは下）の一覧は、走っている台数の多い順の系統。押すとその系統の道とバスを強調し、そこへ寄ります
- バスにマウスを乗せる（タップする）と、系統・行き先・次のバス停・前のバス停を出た時刻・車両番号
- 拡大するとバス停の点と名前が増えます。バス停にマウスを乗せると、そこを通る系統
- 地下鉄の線路（metro_map_cpp と同じ国土数値情報）を目印として薄く描いています

## データ

公共交通オープンデータセンター（ODPT）の公開用 API（キー無し、`https://api-public.odpt.org/api/v4/`）。ブラウザから直接取れます（`Access-Control-Allow-Origin: *`）。

| 何 | どこから | 取り方 |
|---|---|---|
| いま走っているバス | `odpt:Bus?odpt:operator=odpt.Operator:Toei` | 30 秒ごと（ページを開いているあいだ。タブが見えていないときは止める） |
| 系統の道の形とバス停の並び | `odpt:BusroutePattern`（`ug:region` に道に沿った形） | `tools/build_tobus.py` で前もって `data/tobus.txt` に |
| バス停（のりば）の位置と名前 | `odpt:BusstopPole` | 同上 |

### バスの動かし方

ODPT のバスの情報は「どのバス停を何時に出て（`fromBusstopPoleTime`）、次はどのバス停か（`toBusstopPole`）」です。
そのあいだを系統の道の形に沿って、平均 4 m/s（約 14 km/h、信号待ちこみ）で進めます。次のバス停に着いたと言われるまでは、95% の手前で待ちます。
30 秒ごとの更新で位置が先へ進んでいたら、飛ばずに追いつくように少し速く走らせます。

- 時刻表（`odpt:BusTimetable`）は公開用 API では 1000 件で切れてしまうので、使っていません
- バス停を系統の形に載せるとき、周回する系統は同じ場所を 2 回通るので、「前から見て最初に近づいたところ」を取ります（約 2 万か所のうち、形から 150 m 以上離れるのは 144 か所。元の形が途中で欠けている系統）
- 「全体」は 23 区のあたりに合わせます（青梅の系統は 40 km ほど離れているので、真ん中から 20 km より遠いバス停は除く）

## しくみ

| ファイル | やること |
|---|---|
| `tools/build_tobus.py` | ODPT の系統・バス停を `data/tobus.txt` に（形は約 3 m で間引き、バス停は形の上の距離に） |
| `tobus_map.hpp` | 画面をまるごと描く。背景・道・バス停の名前は表示範囲が変わったときだけ、パネルはデータが変わったときだけ描き直し、毎フレームはバスと吹き出しだけ |
| `raster.hpp` | ソフトウェア描画（quake_map_cpp と同じものに、たくさんの線を 1 枚に集めて 1 回で塗る `Mask` を足した） |
| `tools/subset_font.py` | Noto Sans JP を、バス停名・系統名と画面の文言（`tobus_map.hpp` の文字列）の文字だけに絞る（200 KB） |
| `wasm/` | ブラウザ用の入口とビルド |
| `main.cpp` | ネイティブで同じ画面を PNG に描く（取っておいた JSON で確かめる用） |

## ビルド

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# 取っておいた odpt:Bus の JSON を 30 秒おきに届いたものとして流す → out/b_all.png, out/b_zoom.png, out/b_route.png
build/Release/tobus_map.exe data/tobus.txt fonts/NotoSansJP-Bold-subset.ttf out/b 1280 800 bus1.json bus2.json
sh wasm/build.sh   # docs/tb.js, docs/tb.wasm
```

データとフォントを作り直すとき（ODPT から BusstopPole.json と BusroutePattern.json を <dir> に取ってから）:

```sh
python tools/build_tobus.py <dir> ../metro_map_cpp/data/network.txt > data/tobus.txt
python tools/subset_font.py "NotoSansJP[wght].ttf" data/tobus.txt fonts/NotoSansJP-Bold-subset.ttf
```

## 出典とライセンス

- コード: BSD-3-Clause（`LICENSE`）
- バスの位置・系統・バス停: 公共交通オープンデータセンター（ODPT）を通じて東京都交通局が提供するデータを使用（`data/tobus.txt` はそれを加工したもの）。内容の正確さ・完全さは保証されません
- 地下鉄の線路: 「国土数値情報（鉄道データ）」（国土交通省）N02-24 を加工して作成（CC BY 4.0）
- 文字: Noto Sans JP（SIL Open Font License 1.1、`fonts/OFL.txt`）
- stb_truetype / stb_image_write: パブリックドメイン（または MIT）、nlohmann/json 3.12.0: MIT
