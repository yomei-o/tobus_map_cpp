"""バス停名・系統名と画面の文字だけに絞った太字フォントを作る。

    python tools/subset_font.py <NotoSansJP[wght].ttf> data/tobus.txt fonts/NotoSansJP-Bold-subset.ttf

元のフォント: https://github.com/google/fonts/tree/main/ofl/notosansjp （SIL OFL 1.1）
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

src, data, out = sys.argv[1:4]
chars = set(chr(c) for c in range(0x20, 0x7F))
for l in open(data, encoding="utf-8"):
    t = l.split()
    if t and t[0] == "pole":
        chars.update(" ".join(t[4:]))
    elif t and t[0] == "pattern":
        chars.update(" ".join(t[2:]))
# 画面の文字：tobus_map.hpp の文字列をそのまま全部
import re, os
src_hpp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tobus_map.hpp")
for lit in re.findall(r'"([^"]*)"', open(src_hpp, encoding="utf-8").read()):
    chars.update(lit)
chars.update("０１２３４５６７８９都営バスいま走っているの台系統更新読み込み中取れませんでした次はから行き先車両番号遅れ分秒全部表示すべて"
             "停留所路線地下鉄目印出典公共交通オープンデータセンター東京都交通局提供加工作成内容正確保証されません文字選んだを押すと強調"
             "：・（）〜→←↑↓、。「」『』ー－前後駅方面発着系統別いちばん多い順最近確認接続中に通過予定時刻")
f = instancer.instantiateVariableFont(TTFont(src), {"wght": 700})
opt = subset.Options(); opt.layout_features = []; opt.notdef_outline = True; opt.name_IDs = ["*"]; opt.name_languages = ["*"]
opt.hinting = False
s = subset.Subsetter(opt); s.populate(text="".join(chars)); s.subset(f)
f.save(out)
print(len(chars), "chars ->", out)
