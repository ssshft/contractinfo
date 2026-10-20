#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
probe_json.py —— 从交易所的原始响应反推「该在 XxxInfo.cpp 里写哪些字段」。

为什么需要它:
  加一家交易所最耗时的不是写解析，是**搞清楚字段叫什么**。文档常常过时、字段名
  和 ccxt 不一致、数值有时候是字符串有时候是数字。肉眼在 300 KB 的 JSON 里翻，
  翻完还得判断哪个是面值、哪个是 tick、哪个是"精度位数"而不是"步长"。

  这个脚本把响应拆开，按角色给出候选字段和一句可以直接粘进代码的 jf:: 调用。

用法:
    python3 probe_json.py <url|文件路径> [选项]

    python3 probe_json.py https://fapi.bitunix.com/api/v1/futures/market/trading_pairs
    python3 probe_json.py sample.json
    python3 probe_json.py sample.json --field data        # 只看 data 这一层
    python3 probe_json.py sample.json --rows 3            # 每个数组看 3 条样本
    python3 probe_json.py sample.json --json              # 机器可读

它做什么:
  1. 找到所有"记录数组"：对象数组，或数组的数组（Hotcoin fills 那种）
  2. 对对象数组：列出每个字段的类型、样例值，并按角色（symbol/base/quote/tick/
     lot/面值/min/max/状态）打分排序
  3. 对数组的数组：按列报告类型，提示这是"列式记录"
  4. 直接给出建议的 jf:: 调用片段

⚠️ 它只做**建议**，不判断对错。尤其是"精度位数"和"步长"这两种表达，
   必须人工确认 —— 混用会让 tickSize 差 10^n 倍，而且 contractinfo 不会报错。
   见 contractinfo/include/base/InfoHelper.h 的注释。
"""

import argparse
import http.client
import json
import re
import sys
import urllib.request

UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")

# 字段名 -> 角色。**顺序有意义**：先到先得，所以"更具体"的角色必须排在前面。
# 例：symbolStatus 要判成 status 而不是 symbol；contractSize 要判成 value 而不是 symbol。
# 短 token（ts/id/state）一律按大小写敏感匹配，否则 priceProtectScope 里的 "tS"
# 会被当成时间戳 —— 这类误判会让人把字段抄错位置。
ROLE_RULES = [
    # 面值
    ("value",  r"(contractSize|contractVal|contractValue|contractMultiplier|multiplier|"
               r"faceValue|^value$|^contractAmount$)"),
    # 价格精度（可能是步长，也可能是小数位数）
    ("tick",   r"(Tick|pricePrecision|priceScale|priceUnit|priceStep|pricePlace|"
               r"priceEndStep|minStepPrice|quotePrecision)"),
    # 数量精度（同上；注意 minQty / minOrderSize 这类其实是"最小下单量"）
    ("lot",    r"(LotSize|lotSize|stepSize|quantityPrecision|amountPrecision|qtyPrecision|"
               r"basePrecision|volumePrecision|orderQtyPrecision|minQty|minOrderSize|"
               r"minTradeVolume|^lot$|^size$|^qty$)"),
    # 币种拆分（必须锚定整词，否则 basePrecision 会被当成 base）
    ("base",   r"^(base|baseAsset|baseCoin|baseCurrency|underlying|coin|marginCoin)$"),
    ("quote",  r"^(quote|quoteAsset|quoteCoin|quoteCurrency|settle|settleCurrency|"
               r"marginAsset|marginCurrency|currency)$"),
    # 状态
    ("status", r"(Status|State$|^state$|Enable|Switch|Supported|^open$|tradeable)"),
    # 时间（大小写敏感，避免 "tS" 误命中）
    ("time",   r"(Time$|Timestamp|_time$|^ts$|^ts[A-Z_]|Date$|Created|Listed|Launch)"),
    # 编号
    ("id",     r"(^id$|Id$|_id$|^code$|Code$)"),
    # 交易对标识
    ("symbol", r"(Symbol|InstId|instId|^inst$|ContractCode|^Contract$|^name$|Pair|Ticker|^symbol$)"),
    # 上下限（放最后，避免抢走 minQty / minOrderSize）
    ("min",    r"(^min[A-Z]|_min$|^min$|Min[A-Z]|Minimum)"),
    ("max",    r"(^max[A-Z]|_max$|^max$|Max[A-Z]|Maximum)"),
]

ROLE_NOTE = {
    "base":   "标的基础币",
    "quote":  "计价/结算币",
    "symbol": "交易对标识（instId / originInstId 的原料）",
    "value":  "面值候选 —— 1 张 = ? 个 base，跨所常差 10/100 倍",
    "tick":   "价格精度候选 —— ⚠️ 分清是「步长」还是「小数位数」",
    "lot":    "数量精度候选 —— ⚠️ 同上，且要分清单位是「张」还是「币」",
    "min":    "最小限制",
    "max":    "最大限制",
    "status": "可交易状态",
    "id":     "编号",
    "time":   "时间戳",
    "other":  "",
}


def classify(key):
    for role, pat in ROLE_RULES:
        if re.search(pat, key, re.I):
            return role
    return "other"


def kind(v):
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        # 数字形式的字符串很常见（"0.0001"），单独标出来，因为 C++ 侧
        # jf::dbl / jf::i64 能吃两种，但手写 strtod 就只能吃一种。
        if re.fullmatch(r"-?\d+(\.\d+)?([eE][+-]?\d+)?", v.strip() or "x"):
            return "numeric-string"
        return "string"
    if v is None:
        return "null"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    return "?"


def short(v, n=42):
    s = json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v
    s = s.replace("\n", " ")
    return s if len(s) <= n else s[: n - 1] + "…"


def walk_path(root, path):
    cur = root
    for part in path.split("."):
        if not part:
            continue
        if isinstance(cur, dict):
            if part not in cur:
                raise KeyError("路径 %r 里没有 %r" % (path, part))
            cur = cur[part]
        elif isinstance(cur, list):
            cur = cur[int(part)]
        else:
            raise KeyError("路径 %r 在 %r 处不可再下钻" % (path, part))
    return cur


def find_record_arrays(node, path="", out=None, depth=0):
    """找出所有「疑似记录数组」：非空数组，元素是 object，或元素是标量数组。"""
    if out is None:
        out = []
    if depth > 6:
        return out
    if isinstance(node, list):
        if not node:
            out.append((path or "$", "empty-array", 0))
            return out
        if all(isinstance(x, dict) for x in node):
            out.append((path or "$", "object-array", len(node)))
        elif all(isinstance(x, list) for x in node):
            out.append((path or "$", "array-of-arrays", len(node)))
        else:
            for k, x in enumerate(node[:3]):
                find_record_arrays(x, "%s[%d]" % (path, k), out, depth + 1)
    elif isinstance(node, dict):
        for k, v in node.items():
            find_record_arrays(v, (path + "." + k) if path else k, out, depth + 1)
    return out


def report_object_array(name, arr, samples):
    keys = {}
    for rec in arr[: max(samples, 1) * 8]:
        if not isinstance(rec, dict):
            continue
        for k, v in rec.items():
            if k not in keys:
                keys[k] = []
            if len(keys[k]) < samples:
                keys[k].append(v)

    print("  字段清单（共 %d 个）:" % len(keys))
    print("    %-26s %-15s %-8s %s" % ("KEY", "TYPE", "ROLE", "SAMPLE"))
    print("    " + "-" * 92)
    for k, vals in keys.items():
        role = classify(k)
        t = kind(vals[0]) if vals else "?"
        note = ""
        if role in ("tick", "lot") and t in ("number", "numeric-string"):
            try:
                fv = float(vals[0])
                if fv == int(fv) and 0 <= fv <= 18:
                    note = "  ← 疑似「小数位数」而不是步长"
            except (TypeError, ValueError):
                pass
        print("    %-26s %-15s %-8s %s%s" % (k, t, role, short(vals[0]) if vals else "-", note))

    # 按角色给建议
    by_role = {}
    for k in keys:
        by_role.setdefault(classify(k), []).append(k)

    print("\n  建议映射（照抄到 jf::str / jf::dbl / jf::i64 里）:")
    order = ["symbol", "base", "quote", "value", "tick", "lot", "min", "max", "status"]
    for role in order:
        cand = by_role.get(role)
        if not cand:
            print("    %-8s —（没找到，需要人工确认）" % role)
            continue
        # 同一角色多个候选时全部列出，第一个加 * 标记
        fmt = ", ".join('"%s"' % c for c in cand[:4])
        print("    %-8s { %-46s }   %s" % (role, fmt, ROLE_NOTE.get(role, "")))

    # 数值型字段统一用 jf::dbl，字符串型用 jf::str —— 给一句提醒
    numeric_str = [k for k, vals in keys.items() if vals and kind(vals[0]) == "numeric-string"]
    if numeric_str:
        print("\n  ⚠️ 这些字段是**数字形式的字符串**，C++ 侧必须用 jf::dbl/jf::i64（能兼容两种），")
        print("     直接 row[\"x\"].get_double() 会失败：%s" % ", ".join(numeric_str[:8]))
    num = [k for k, vals in keys.items() if vals and kind(vals[0]) == "number"]
    if num:
        print("  ℹ️ 这些字段是**裸数字**，用 jf::str 取会失败：%s" % ", ".join(num[:8]))


def report_array_of_arrays(name, arr, samples):
    print("  ⚠️ 这是「列式记录」：每行是一个数组，不是对象。")
    print("     典型如 Hotcoin 的 fills: data:[[px, sz, \"long\"|\"short\", ts, id], ...]")
    print("     解析时要按**下标**取值，且不能按对象字段访问。")
    cols = 0
    for r in arr[:samples]:
        cols = max(cols, len(r))
    print("\n  行数 %d，最多 %d 列" % (len(arr), cols))
    for c in range(cols):
        types = []
        vals = []
        for r in arr[: max(samples, 1) * 4]:
            if c < len(r):
                types.append(kind(r[c]))
                if len(vals) < 2:
                    vals.append(r[c])
        print("    [%d] %-16s %s" % (c, "/".join(sorted(set(types))), ", ".join(short(v) for v in vals)))


def main():
    ap = argparse.ArgumentParser(add_help=True, description="从原始 JSON 反推字段映射")
    ap.add_argument("source", help="URL 或本地 JSON 文件")
    ap.add_argument("--field", default="", help="只看这个路径下的内容，如 data 或 data.list")
    ap.add_argument("--rows", type=int, default=2, help="每个字段取几条样本（默认 2）")
    ap.add_argument("--json", action="store_true", help="机器可读输出")
    ap.add_argument("--header", action="append", default=[], metavar="K:V",
                    help="额外请求头，可重复，如 --header 'X-Api-Key:xxx'")
    args = ap.parse_args()

    if re.match(r"^https?://", args.source):
        req = urllib.request.Request(args.source)
        req.add_header("User-Agent", UA)
        req.add_header("Accept", "application/json")
        for h in args.header:
            if ":" in h:
                k, v = h.split(":", 1)
                req.add_header(k.strip(), v.strip())
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                # ⚠️ 不能直接 r.read()：交易所经常不带 Content-Length 就断连，
                # 会抛 IncompleteRead，但 e.partial 里其实已经有完整 body。
                chunks = []
                try:
                    while True:
                        b = r.read(65536)
                        if not b:
                            break
                        chunks.append(b)
                except http.client.IncompleteRead as e:
                    chunks.append(e.partial)
                raw = b"".join(chunks)
                ctype = r.headers.get("Content-Type", "")
                status = r.status
        except Exception as e:
            print("请求失败: %s" % e, file=sys.stderr)
            return 1
        print("HTTP %d  %s  %d bytes" % (status, ctype, len(raw)))
        text = raw.decode("utf-8", "replace")
    else:
        with open(args.source, "rb") as f:
            raw = f.read()
        text = raw.decode("utf-8", "replace")
        print("%s  %d bytes" % (args.source, len(raw)))

    try:
        doc = json.loads(text)
    except json.JSONDecodeError as e:
        print("不是合法 JSON: %s" % e, file=sys.stderr)
        print("前 400 字节: %s" % text[:400], file=sys.stderr)
        return 1

    if args.field:
        try:
            doc = walk_path(doc, args.field)
        except (KeyError, ValueError, IndexError) as e:
            print(str(e), file=sys.stderr)
            return 1
        print("下钻到: %s" % args.field)

    # 顶层形状
    if isinstance(doc, dict):
        print("\n顶层 object，%d 个键: %s" % (len(doc), ", ".join(list(doc)[:14])))
    elif isinstance(doc, list):
        print("\n顶层 array，%d 个元素" % len(doc))
    else:
        print("\n顶层是标量: %r" % (doc,))
        return 0

    arrays = find_record_arrays(doc)
    if not arrays:
        print("\n没找到记录数组。试试点名 --field 下钻，或用 --field data 之类。")
        return 0

    print("\n找到 %d 个候选数组:" % len(arrays))
    for p, t, n in arrays:
        print("  %-40s %-18s %d 条" % (p or "$", t, n))

    if args.json:
        out = {"source": args.source, "arrays": []}
        for p, t, n in arrays:
            out["arrays"].append({"path": p, "kind": t, "count": n})
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0

    for p, t, n in arrays:
        if t == "empty-array":
            print("\n=== %s === 空数组（外壳对了，是筛选条件的问题）" % p)
            continue
        if n == 0:
            continue
        try:
            arr = walk_path(doc, p) if p != "$" else doc
        except (KeyError, ValueError, IndexError):
            continue
        print("\n=== %s ===  %s，%d 条" % (p, t, n))
        if t == "object-array":
            report_object_array(p, arr, args.rows)
        elif t == "array-of-arrays":
            report_array_of_arrays(p, arr, args.rows)

    print("\n下一步:")
    print("  1. 把上面的建议映射写进 contractinfo/src/<exch>/<Exch>Info.cpp")
    print("  2. 精度字段务必人工确认是「步长」还是「小数位数」—— 见 include/base/InfoHelper.h")
    print("  3. 面值跨所对照：同一 base 的 value 差 10/100 倍是常态，差 1000 倍要怀疑单位")
    print("  4. 上机跑 ./InfoCheck --exchange <EXCH> 验证")
    return 0


if __name__ == "__main__":
    sys.exit(main())
