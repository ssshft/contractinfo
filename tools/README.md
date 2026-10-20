# contractinfo/tools

六个 C++ 工具 + 一个 python 脚本，按「读 / 写 / 裸段 / 组件 / 自测」分工。
**加一家交易所时按下面的流程走，10 分钟能验完。**

| 工具 | 视角 | 回答的问题 | 依赖 |
|---|---|---|---|
| `InfoCheck` | 写侧 | 我们**写进** `InstrumentInfo` 的东西对不对？ | 链 `ContractInfoCore`，联网 |
| `GetInfo` | 读侧 | `db` **能查到**什么？ | 纯头文件，读 SHM/Redis |
| `DumpInfoShm` | 裸段 | SHM 里到底有什么？ | 纯头文件 |
| `TestInfoShm` / `BenchInfoShm` | 组件 | SHM 读写对不对 / 有多快 | 纯头文件 |
| `selftest_rules` | 自测 | **规则引擎自己对吗？**（不联网、不读 SHM、纯 std） | 只要编译器 |
| `probe_json.py` | 辅助 | 交易所的响应里**字段叫什么**？ | 只要 python3 |

排查链：`InfoCheck`（写）→ `DumpInfoShm`（存）→ `GetInfo`（读）。哪一段断了，问题就定位在那一段。

---

## 新增一家交易所的标准流程

```
1. 探字段          python3 tools/probe_json.py <接口URL>
2. 写解析          include/<exch>/<Exch>Info.h + src/<exch>/<Exch>Info.cpp
3. 注册            见下方「必须改的 4 处」
4. 编译            make -j
5. 验写侧          ./bin/InfoCheck --exchange <EXCH> --sample 3
6. 验读侧          ./bin/GetInfo --exchange <EXCH> --stats
7. 存基线          ./bin/InfoCheck --out baseline_$(date +%F).json
```

第 5 步输出全 PASS 才算接完。有 FAIL 就看下面「检查项速查」。

### 必须改的 4 处

1. `include/data_struct.h`
   `enum ExchangeType` **只能往后追加**（枚举值会进 SHM 和 redis 复合键，插中间会让在跑的
   db/tb 认错交易所）；同一次提交里把 `ExchangeTypeEnum2StrMap` 和 `ExchangeTypeStr2EnumMap`
   都加上。**漏了不报错**，复合键会退化成 `.USDT_SWAP.BTC-USDT` 并跨所撞键。
2. `include/operation/ExchangeInfoFactory.h` 对应的 `kEntries` 表 —— 加一行。
   生产路径和所有工具都从这张表取，加一次两边同时生效。
3. `CMakeLists.txt`：`aux_source_directory(... DIR_XXX_SRCS)` + 把 `${DIR_XXX_SRCS}`
   加进 `ContractInfoCore` 的源列表。**两处都要加**，只加一处是 undefined reference。
4. `etc/contract_info.json` 的 `exchangeList` 里加名字。

---

## InfoCheck

写侧的验收器。直接构造真实的 `XxxInfo` 调 `syncExchangeInfo()`，对结果跑 32 项不变量
（25 项逐条记录 + 7 项全局：R1–R3 / F1–F2 / G1 / H1）。

```bash
./bin/InfoCheck                                  # 配置里所有交易所
./bin/InfoCheck --exchange BITUNIX               # 只跑一家
./bin/InfoCheck --exchange BITUNIX,PHEMEX        # 跑几家
./bin/InfoCheck --exchange BITUNIX --sample 5    # 顺便打印 5 条明细
./bin/InfoCheck --json > snap.json               # 机器可读（= --out 的内容）
./bin/InfoCheck --out snap.json                  # 存基线
./bin/InfoCheck --diff snap.json                 # 与基线比：下架 / 新增 / 精度变化
./bin/InfoCheck --from-json snap.json            # 不联网，直接对快照跑全部检查
./bin/InfoCheck --db-config ../db/etc/small_swap_db.json
                                                 # 额外检查 db 配置里的对子能否查到
./bin/InfoCheck --strict                         # WARN 也算失败（CI 用）
```

退出码：`0` 通过 / `1` 有 FATAL 或 ERROR / `2` 用法或配置错误 / `3` 只有 WARN 且开了 `--strict`。

`--from-json` 的意义：快照可以在**部署机**上生成，然后在**开发机**上离线跑检查，
不需要网络也不需要编译环境。

### 检查项速查

| 编号 | 检查 | 失败意味着 |
|---|---|---|
| R1 | `enum_map` | `ExchangeType`/`InstType` 的枚举与字符串表不一致、有空值、有重复、有编号空洞 |
| R2 | `config` | 配置缺 `needMagnifyNum`（**`BaseInfo` 构造会 `std::stoi("")` 抛异常，进程直接 terminate**） |
| R3 | `supported` | `exchangeList` 里的名字在注册表里没有 → 该所静默零数据 |
| A1 | `fetch` | 拉到 0 条。接口挂了 / 字段名变了 / 过滤条件写错了 |
| B1–B4 | `instId` `origin` `base` `quote` | 字段没填。`originInstId` 空 = db 回查不到 |
| B5 | `distinct` | `base == quote` |
| B6 | `margin` | 衍生品没有结算币 |
| B7 | `capchar` | 字段写满 `char[32]`/`char[16]` —— `strncpy` 不保证 NUL 结尾，下游 `strcpy` 越界 |
| B8 | `whitespace` | 标识字段里有空格/制表符，db 的 symbol 匹配会全失败 |
| B9 | `unique` | 同一 instType 下 `instId` 或 `originInstId` 重复 |
| B10 | `enumMatch` | **`exchangeTypeEnum` 填成了别家** —— 复制粘贴忘改名。数据会挂到别人名下，零报错 |
| C1 | `pow10` | `pricePow10 != 10^priceDigits` |
| C2 | `tickInt` | `tickSizeInt != round(tickSize × pricePow10)` |
| C3 | `lotInt` | 同上，数量侧 |
| C4/C5 | `tickPos` `lotPos` | `tickSize`/`lotSize` 不是正数 |
| **C6** | **`pxRound`** | **`quantize(tickSize) == 0`** —— 挂单价会被截成 0，`quantize` 本身不报错 |
| **C7** | **`szRound`** | 同上，下单量被截成 0 |
| C8 | `pxGrid` | `1×/7×/12345×` tick 量化后偏离网格 |
| D1 | `minMax` | `minSize > maxSize` |
| D2 | `minPos` | `minSize == 0`（提示级；下游用 `lotSize` 兜底） |
| D3 | `amount` | `minAmount < 0` |
| E1 | `value` | 面值 `<= 0` |
| E2 | `magnify` | `magnifyNumber × reduceNumber != 1` |
| E3 | `basePfx` | base 是 `1000CHEEMS` 这种但 `magnifyNumber` 不是 1000 |
| F1 | `xValue` | 同一合约在不同所的面值比值**不是 10 的整数次幂**（单位理解错） |
| F2 | `xTick` | 同一合约 tick 差 >1000 倍 |
| G1 | `dbConfig` | `small_swap_db.json` 里写的对子在这里查不到 → db 那个 unit 起不来 |
| H1 | `diff` | 与基线相比有下架 / 新增 / `tickSize`·`lotSize`·`value`·`magnify` 变化 |

C6/C7 是最关键的两条：`(tickSize, pricePow10, tickSizeInt)` 三者不自洽时，
`crypto::quantize()` 会把价格算成 0，**不抛异常、不打日志**，直接变成废单。

### 失败时的第一反应

- `A1` → 看 `logs/InfoCheck_*.log`，解析器把 `LOG_ERROR` 都写在那里（HTTP 状态 / 解析失败 / 字段缺失）
- `B7` → 交易所的 symbol 太长，或者用 `snprintf` 时把 size 传成了 `sizeof(指针)`
- `C2/C3/C6` → 十有八九是把「小数位数」当成「步长」用了，或者反过来。
  看 `include/base/InfoHelper.h` 顶部关于 `priceFromStep` / `priceFromFixed` 的注释
- `E1/F1` → 面值单位：是「张」还是「币」还是「USDT 名义价值」
- `G1` → `contractinfo` 的 `instId` 拼法和 `db` 配置对不上（最常见的是分隔符 `-` vs `_`、大小写）

---

## GetInfo

读侧。走 `SecurityManager`，和 `db` 完全同一条路径。

```bash
./bin/GetInfo                                  # 全量表格
./bin/GetInfo --exchange BITUNIX
./bin/GetInfo --exchange BITUNIX --inst-type USDT_SWAP --limit 10
./bin/GetInfo --inst-id BTC-USDT               # 精确查一条（instId / originInstId 都行）
./bin/GetInfo --inst-id BTCUSDT --exchange BITUNIX --inst-type USDT_SWAP
./bin/GetInfo --stats                          # 总条数 / 每家条数 / instType 分布
./bin/GetInfo --keys                           # 只打复合键，查「撞键 / 空串开头」
./bin/GetInfo --json
./bin/GetInfo --resolve pairs.txt              # 批量：每行 EXCH,INSTTYPE,INSTID
./bin/GetInfo --shm-name /my_seg               # 覆盖 BTS_INFO_SHM_NAME
```

`--resolve` 模拟 `db` 启动时干的事：拿着配置里的三元组去查，查不到就打一行
`cannot find originInstId` 并跳过那个 unit。**未命中数 = 会有多少个对子静默失效。**

`--keys` 的用途：正常应该长这样

```
BITUNIX.USDT_SWAP.BTC-USDT.INSTRUMENT_INFO
```

如果看到 `.USDT_SWAP.BTC-USDT.INSTRUMENT_INFO`（空串开头），说明
`ExchangeTypeEnum2StrMap` 缺了这个枚举值 —— 而且所有缺失的交易所会**撞同一个键**。

---

## probe_json.py

加新交易所时最费时间的一步是搞清楚字段叫什么。这个脚本把响应拆开，按角色给出候选字段，
并直接给出能粘进代码的 `jf::` 调用。

```bash
python3 tools/probe_json.py https://fapi.bitunix.com/api/v1/futures/market/trading_pairs
python3 tools/probe_json.py sample.json --field data
python3 tools/probe_json.py sample.json --rows 3
```

输出示例（Bitunix 实测）：

```
=== data ===  object-array，781 条
    KEY                        TYPE            ROLE     SAMPLE
    symbol                     string          symbol   BTCUSDT
    base                       string          base     BTC
    quote                      string          quote    USDT
    basePrecision              number          lot      4  ← 疑似「小数位数」而不是步长
    quotePrecision             number          tick     1  ← 疑似「小数位数」而不是步长

  建议映射（照抄到 jf::str / jf::dbl / jf::i64 里）:
    symbol   { "symbol"                                       }
    base     { "base"                                         }
    quote    { "quote"                                        }
    value    —（没找到，需要人工确认）
    tick     { "quotePrecision"                               }
    lot      { "minTradeVolume", "basePrecision"              }
```

它**只给建议，不判断对错**。尤其是「精度位数 vs 步长」必须人工确认 —— 混用会让
`tickSize` 差 10ⁿ 倍，而且 `contractinfo` 不会报错。脚本会标出「疑似小数位数」提示这一点。

它还能识别**列式记录**（数组的数组），比如 Hotcoin 的 fills：
`data:[[px, sz, "long"|"short", ts, id], ...]`。按对象字段去取会一个都取不到。

---

## selftest_rules（规则引擎的自测）

`InfoCheckRules.h` 是纯 std 的（只依赖 `precision_util.h`），所以**本机就能编、就能跑**：

```bash
clang++ -std=c++17 -Wall -Wextra -I../include -I../../include \
        -o /tmp/selftest_rules selftest_rules.cpp && /tmp/selftest_rules
```

它做两件事：

1. **3 组合法数据必须全绿**（BTC-USDT 单条；六家混排的真实精度组合，含 WEEX DOGE tick `1e-5`、
   ETH lot `0.001`、币本位 tick `0.5` + `margin=BTC`、`1000CHEEMS` 已剥前缀 + `magnify=1000`、
   SPOT 空 margin；`needMagnifyNum=0` 时**保留前缀不算错**）。
2. **26 组单点破坏必须被对应编号抓到**（改一个字段，只有那一条红）。

**为什么值得单独跑**：`InfoCheck` 本身要链 `ContractInfoCore`、要联网，一旦误报你分不清是
"交易所数据怪" 还是 "规则写错了"。这个自测把规则引擎和真实数据解耦 —— 它绿了，
`InfoCheck` 报出来的每一条都能当真。当前 **29/29 通过**。

改动 `InfoCheckRules.h` 里任何阈值/判据后，**先跑它再上部署机**。

---

## 本机能做什么、不能做什么

本机（macOS）**编不了** contractinfo：缺 `boost` / `rapidjson` / `simdjson` / `fmt` / `cpp_redis`。

- ✅ 能改代码、做静态检查、跑 `probe_json.py`
- ✅ 能跑 `--from-json` 的离线检查（只要有部署机生成的快照）
- ❌ 不能 `make`，不能跑 `InfoCheck` / `GetInfo` 的联网/读 SHM 模式

所以标准动作是：**本机改代码 → 部署机 `make -j` → 部署机跑 `InfoCheck`**。
编译报错优先怀疑 `simdjson` 的 `dom` API 和 `fmt::format` 对 `std::string_view` 的处理。
