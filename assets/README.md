# assets — 容器层测试素材

> 入库目录。放**确定性**的 `.clv` 素材，供端到端测试与人工核对使用。
> 与 `demo/` 解耦：demo 自己现场生成文件，**不读本目录**；本目录也不依赖 demo 才能产出。

## 目录

```
assets/
├── README.md
└── fixtures/
    ├── minimal.clv
    ├── neg_dts.clv
    └── subtitle_ext4.clv
```

生成脚本按项目约定放在仓库的 `script/` 下：`script/gen_fixtures.py`。

## 三份素材各测什么

| 文件 | 内容 | 考察点 |
|---|---|---|
| `minimal.clv` | 一条视频流、两个单包帧、带索引 | 最小合法文件：头 / 描述符 / 包 / 索引 / 尾齐全，CRC 全对 |
| `neg_dts.clv` | 源首包 dts = −40；第二帧 700 B 按 300 切三片 | **写方平移义务**（落盘首包 dts = 0）+ **每片完整头**的分片与片序连续性 |
| `subtitle_ext4.clv` | 视频 + 字幕双流，字幕挂 `ext_type = 4` 样式块，四帧 | 扩展块链、跨流索引的统一时间轴次序、dts 相等时按 `stream_id` 的 tie-break、分片重组 |

## 生成与确定性

```
python script/gen_fixtures.py            # 默认写 assets/fixtures/
python script/gen_fixtures.py --out DIR  # 写到别处（比对用）
```

**确定性是硬要求**：字节流只能是输入的纯函数，不得含时间戳、路径、随机数或任何随环境变化的量。
入库前置条件 = 连跑两次产出逐字节一致（`diff -r` 无差异）。

生成器是**独立实现**（不复用 C++ 侧代码），所以它同时是一份交叉验证：Python 写、库的读方读，
两边对不上就说明有一方偏离了格式。核对方式：

```
demo_local/bin/clv_demo_container.exe info assets/fixtures/neg_dts.clv
```

## 冻结值（重跑必须复现，否则先查确定性再入库）

| 文件 | 字节数 | 整文件 CRC-32/MPEG-2 | SHA-256（前 16） |
|---|---|---|---|
| `minimal.clv` | 335 | `F3CFBDEB` | `521df20ba0a2cdc3` |
| `neg_dts.clv` | 1081 | `9DED3CBB` | `b8f10c680703eae5` |
| `subtitle_ext4.clv` | 1724 | `FB942777` | `7ad9c6e27fc9d180` |

**版本号变更必须重跑**：文件头带 `version_major` / `version_minor`（现 1 / 0），CLV 版本或结构口径一变，
上面这些字节就会变。此时**重跑 `script/gen_fixtures.py` 并更新本表**，别手改素材文件、别留着旧值。
CRC-32/MPEG-2 参数集：poly `0x04C11DB7`、init `0xFFFFFFFF`、不反射、xorout 0（与 `src/core/crc32.h` 一致）。
