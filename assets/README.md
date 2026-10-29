# assets — 测试素材

> 入库目录。放**确定性**素材，供端到端测试与人工核对使用。
> 与 `demo/` 解耦：demo 自己现场生成文件，**不读本目录**；本目录也不依赖 demo 才能产出。

## 目录

```text
assets/
├── README.md
├── fixtures/
│   ├── minimal.clv
│   ├── neg_dts.clv
│   ├── subtitle_ext4.clv
│   ├── subtitle_pipeline.clv
│   ├── demo_av_1080p_q32.clv    本库编码 + 封装产出的双轨演示件：1080p / 30 fps / 3.5 s / 40.2 MiB
│   ├── demo_avs_360p_q32.clv    本库编码 + 封装 + 字幕并入的三轨演示件：640×360 / 30 fps / 3 s / 5.5 MiB
│   ├── sample-10s.mp4      ┐
│   ├── sample-20s.mp4      ├ 外部示例视频，编解码与画质测量用
│   ├── sample-30s.mp4      ┘
│   ├── sample-15s-id3v2.mp3     外部示例音频原件（带 ID3v2 注释与封面）
│   ├── sample-15s.flac          由上面那支转换而来，注释与封面都带过来；元数据解读面的真件载体
│   └── provenance.txt      来源、整件 SHA-256、取帧命令、工具版本
├── fonts/
│   ├── NotoSansCJKsc/      Noto Sans CJK SC 7 字重 + OFL LICENSE（字幕渲染判据的基准族）
│   ├── NotoNaskhArabic-v2.021/   Noto Naskh Arabic 25 份字体件（4 字面 × 多种构建）+ OFL 与说明件（阿语 = 基准族第二份）
│   ├── noto-emoji/         NotoColorEmoji.ttf 一份 + OFL LICENSE（表情符号面取样件，非基准族）
│   └── provenance.txt      来源、整件 SHA-256、字体元数据（三份字体各一节）
└── synth/
    └── provenance.txt      自写生成序列的定义与哈希（字节不落本目录，见下）
```

生成脚本按项目约定放在仓库的 `scripts/` 下：容器素材 = `scripts/gen_fixtures.py`，
门面基准序列 = `scripts/gen_synth_sequence.py`。

## 素材按尺子分类

| 类别 | 素材 | 量的是 | 判据形态 |
|---|---|---|---|
| 真实内容 | `fixtures/sample-{10s,20s,30s}.mp4` | **画质**：误差随量化档的走势 | 有浮动的对比带（同视频纵向比） |
| 自写生成 | `synth/provenance.txt` 定义的序列 | **正确性**：已知图样上的行为与逐字节全等 | 定值，无浮动 |
| 真实 FLAC 件 | `fixtures/sample-15s.flac`（+ 它的 mp3 原件） | **元数据解读面**：注释块 11 条逐条取回与查键、图标块九项字段、承载块整块往返；顺带给可听音频腿供料 | 定值（块表四块的类型与字节数、vendor 串、图标九项字段、电平 −21.7 / −5.9 dB）|
| 本库自产双轨件 | `fixtures/demo_av_1080p_q32.clv` | **端到端**：编码 → 封装 → 解回 → 可播，视音频在**同一个文件**里 | 定值（143 包、音频解回与源逐字节全等、三平面 PSNR）|
| 本库自产三轨件 | `fixtures/demo_avs_360p_q32.clv` | **三轨同步**：视频 + 音频 + 字幕在同一文件里按 pts 连续出，字幕层的实际叠加面 | 定值（90 / 33 / 3 包，字幕 3 块 3 行，播放侧 `frames = 90` 且 `dropped = 0`；`shown` 是字幕层出画帧数，等于 90 减去执行侧按 `drop_dup_hint` 削掉的帧，不钉死单值）|
| 字体（基准族） | `fonts/NotoSansCJKsc/*.otf` | **字幕渲染**：CJK 字形、布局、回退与字重覆盖 | 同平台同字体 golden；跨平台只断布局 / bbox / 覆盖率，像素差异允许 |
| 字体（基准族第二份 · 阿语） | `fonts/NotoNaskhArabic-v2.021/NotoNaskhArabic/full/otf/{Regular,Bold}.otf` | **阿语（RTL）面**：Bidi 分趟、整形按位置换字形、阿语覆盖率 | 只断「shaped 字形号 ≠ nominal 字形号至少一处」与 `.notdef = 0`；**不做 golden 像素比对**，它不是基准族 |
| 字体（表情符号面） | `fonts/noto-emoji/NotoColorEmoji.ttf` | **emoji 面**：彩色位图字形、回退链走到 emoji 档时有真件可指、`style` 硬过滤只有一档 Regular | 位图件，**跨平台像素 golden 不成立**；判据待表情面开工时补，本版只登记形态 |

尺子之间不混用。跨视频的 PSNR 绝对值**不作横向比较**——外部素材的色彩标记四项全为 unknown，
各自只与自己不同量化档的序列比；自写序列的彩条 Y/Cb/Cr 是矩阵算出来的定值。

**`synth/` 为什么不放字节**：那条序列是 1080p / 30 fps / 30 s 的原始帧流，六段共 2 799 360 000 B（= 段字节 466 560 000 × 6，算式见 `synth/provenance.txt`「画面」一节），
提交进仓库既没必要也不可能。它由 `scripts/gen_synth_sequence.py` 按 `synth/provenance.txt`「复现」一节的命令**逐段**复现（命令行没有一次出全部的模式），
登记表里的每段 SHA-256 就是「复现对了」的判据——素材的定义 = 生成器 + 哈希。

**另有一段不属于节目的补材**：生成器的段 7 = **渐变面**（对角空间坡 + 时域电平坡，全整数算式），
补的是画质度量三类内容形态（静止面 / 动态面 / 渐变面）里原六段缺的渐变一类。它不排进 30 s 节目、不与音频对齐，
登记同样走「生成器 + 整段 SHA-256」（见 `synth/provenance.txt` 的段 7 节）。
加它之后把段 1..6 整段重生成对过哈希，**六条逐位相同**，所以既有素材的登记值一个没作废。

## 四份自制 `.clv` 素材各测什么

| 文件 | 内容 | 考察点 |
|---|---|---|
| `minimal.clv` | 一条视频流（挂 `ext_type = 3` 参数块）、两个单包帧、带索引 | 最小合法文件：头 / 描述符 / 扩展块 / 包 / 索引 / 尾齐全，CRC 全对 |
| `neg_dts.clv` | 源首包 dts = −40；第二帧 700 B 按 300 切三片；视频流挂 `ext_type = 3` 参数块 | **写方平移义务**（落盘首包 dts = 0）+ **每片完整头**的分片与片序连续性 |
| `subtitle_ext4.clv` | 视频 + 字幕双流，视频挂 `ext_type = 3` 参数块、字幕挂 `ext_type = 4` 样式块（扩展块共两条），四帧 | 扩展块链（两条链各一块）、跨流索引的统一时间轴次序、dts 相等时按 `stream_id` 的 tie-break、分片重组 |
| `subtitle_pipeline.clv` | 视频 + 字幕双流，视频挂 `ext_type = 3` 参数块、字幕挂 `ext_type = 4` 样式块（字号 4.0% 屏高 / 白字 / 1 px 黑描边 / 字族链）；三个真实字幕包覆盖 0–3 s，第三包带 `[\emoji …]` 指令段 | 字幕包外层与块语法（块 / 属性 / 文本行 / 时间窗）、样式块进覆盖链、跨包 ID 与 start 单调、`\emoji` 路由段；M5 字幕管线示例的默认素材 |

`ext_type = 3` 参数块自容器格式版本 2 起对**每条视频流必选**（口径见随仓库发行的 `docs/容器规范.md` 的扩展块一节）：
载荷 8 字节 `max_b_frames / quant_precision / ext_tools / reserved / width LE16 / height LE16`，`width` 与 `height` 不得为 0。
这四份素材的视频载荷是合成图案、没有真画面对应，故尺寸取一个合法非零值 352×288（生成器里注释同源）。

## 三轨演示件 `demo_avs_360p_q32.clv`（2026-10-26）

视频 + 音频 + 字幕三轨同文件，用于 M5 字幕管线示例（`clv_demo_subtitle`）的自动播放：
视频/音频由本库公开面编码与封装，字幕流由 `subtitle_pipeline.clv` 的载荷经 `scripts/mux_avs.cpp` 并入（公开读/写面）。

| 项 | 值 |
|---|---|
| 视频 | 640 × 360、30 fps、90 帧 = 3.0 s、q = 32、`quant_precision = 7`、I 帧间隔 15 |
| 音频 | 44.1 kHz / 2 ch / 16-bit，3.0 s（132 300 样本），FLAC |
| 字幕 | 3 包 / 3 块，覆盖 0–3 s；样式块 ext_type = 4（4.0% 屏高、白字、1 px 黑描边、Noto Sans CJK SC） |
| 体积 | 5 717 545 B |

配方（在仓库根执行）：

```text
ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 1080x720 -i <1080x720 300 帧 YUV> -frames:v 90 -vf scale=640:360 -f rawvideo build/av360.yuv
ffmpeg -y -i assets/fixtures/sample-15s.flac -t 3 -f s16le -acodec pcm_s16le -ac 2 -ar 44100 build/av360.pcm
mtav enc build/plan360.txt build/mtav360out && mtav mux build/plan360.txt build/mtav360out   （plan：W640 H360 QP7，UNIT 0 90 q32 ird15）
mux_avs build/mtav360out/av_000.clv assets/fixtures/subtitle_pipeline.clv assets/fixtures/demo_avs_360p_q32.clv
```

前两步的编码中间件同 `demo_av_1080p_q32.clv` 的 `mtav` 链；第三步只做流的搬运与字幕并入。
入库值见上面「冻结值」表；`mtav` 与 `mux_avs` 都是公开 C ABI 的驱动件，不进构建面。

## 第四份 `.clv`：`demo_av_1080p_q32.clv`（本库自产的双轨演示件）

前三份由 `scripts/gen_fixtures.py`（Python 独立实现）写出，量的是**读方**；这一份反过来——
它由**本库的编码与封装面**产出，量的是「编得出、封得进、解得回、播得出」整条链，
所以它的期望值全部来自实测而不是设计值。配方、逐件哈希与入库前读数在 `fixtures/provenance.txt`。

| 项 | 值 |
|---|---|
| 画面 | 1 920 × 1 080、**30 fps**、105 帧 = 3.5 s、源 = `sample-30s.mp4`（帧率 30/1 为整数，正对 `1/30` 时间基） |
| 音频 | 源 = `sample-15s.flac` 解出的 16-bit / 44 100 Hz / 2 ch PCM，取前 3.5 s |
| 编码档 | 有损 `q = 32`、`quant_precision = 7`、I 帧间隔 15、系数载体取法 1 档（构建默认档） |
| 结构 | **单文件双流单段**：视频流 `ext_type = 3` 参数块 + 音频流 `ext_type = 5` FLAC STREAMINFO，143 包交错 |
| 体积 | 42 137 405 B = 40.2 MiB ⇒ 压在 50 MiB 线内；50 MB ÷ 实测 398 124 B/帧 = 125 帧，这是 1080p 有损档的硬顶 |
| 音频元数据 | **不带**：演示件里的音频只作可播负载用，注释块与封面在这条链上没有读者，那两块留在 `sample-15s.flac` 本身 |
| 复现局限 | 驱动件在库内的 `scripts/mtav.cpp`，但不进构建面（没有 CMake 目标构建它）⇒ 复现本件仍需手工编译该件；复现力 = 命令 + 输入哈希 + 产物哈希，不等于「克隆即可重跑」 |

> 系数载体有两种写法：定长档与法 1 档（先写结束标志 EOB，再用位数递增的 Exp-Golomb 编码写幅值）；具体用哪一档由视频帧头第 5 到第 7 位声明，本库默认法 1。

## 字幕测试字体：NotoSansCJKsc（2026-10-22 入库）

来源：`https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/08_NotoSansCJKsc.zip`（发布标签 `Sans2.004`）。
入库对象 = `fonts/NotoSansCJKsc/` 下 **7 个 OTF + `LICENSE`**；逐件 SHA-256、family / subfamily / `usWeightClass` 与许可见 `fonts/provenance.txt`。

| 字重 | 文件 | 字节数 | family / subfamily | `usWeightClass` |
|---|---|---|---|---|
| Thin | `NotoSansCJKsc-Thin.otf` | 14 896 688 | `Noto Sans CJK SC Thin` / `Regular` | 100 |
| Light | `NotoSansCJKsc-Light.otf` | 16 329 648 | `Noto Sans CJK SC Light` / `Regular` | 300 |
| DemiLight | `NotoSansCJKsc-DemiLight.otf` | 16 414 760 | `Noto Sans CJK SC DemiLight` / `Regular` | 350 |
| Regular | `NotoSansCJKsc-Regular.otf` | 16 437 364 | `Noto Sans CJK SC` / `Regular` | 400 |
| Medium | `NotoSansCJKsc-Medium.otf` | 16 523 632 | `Noto Sans CJK SC Medium` / `Regular` | 500 |
| Bold | `NotoSansCJKsc-Bold.otf` | 17 002 248 | `Noto Sans CJK SC` / `Bold` | 700 |
| Black | `NotoSansCJKsc-Black.otf` | 17 797 908 | `Noto Sans CJK SC Black` / `Regular` | 900 |
| 合计 | 7 OTF + `LICENSE` | **115 406 549 B = 110.1 MiB** | — | — |

- 用途：字幕渲染器的**测试 / 参考字体**，固定 CJK 字形与布局；**不是库的运行时依赖**。产品路径仍由宿主 / CAL 的系统字体解析（Windows DirectWrite），`font_dir` 指到本目录时才按文件建 face。
- 默认 `Regular`（family `Noto Sans CJK SC`，weight 400）；`Bold` 作粗体对照；7 件覆盖 100 / 300 / 350 / 400 / 500 / 700 / 900。
- **family 名不全是同一个**：Regular / Bold 的 family = `Noto Sans CJK SC`；Thin / Light / DemiLight / Medium / Black 的 family 名带字重后缀。测试的 `font-family` 链与 `font-weight` 选择按 family / subfamily + `usWeightClass` 映射，不按文件名猜。
- 许可 = SIL OFL 1.1；`LICENSE` 随目录入库；本仓未改名、未重打包、未子集化（逐件 SHA-256 见 `fonts/provenance.txt`）。单件均低于 GitHub 的 50 MiB 警告线，按不开 LFS 的口径以普通 blob 入库。
- 判据：同平台同字体同 CAL 构型做 golden；跨平台只断布局 / bbox / 覆盖率，像素差异允许。哈希只证明「用的是同一份字体字节」，不证明跨平台像素一致。

## 字幕阿语取样字体：NotoNaskhArabic（2026-10-22 入库）

来源：`https://github.com/notofonts/arabic/releases/download/NotoNaskhArabic-v2.021/NotoNaskhArabic-v2.021.zip`
（仓库 `notofonts/arabic` · 标签 `NotoNaskhArabic-v2.021` · 发布件 2 922 060 B · SHA-256 `6c050ab9…4514032` · 发布页 `published_at = 2026-01-16T09:25:27Z`）。
入库对象 = `fonts/NotoNaskhArabic-v2.021/` 下 **25 份字体件 + 5 份说明与许可件**；逐件 SHA-256、family / subfamily / `usWeightClass` / 字形数与 GSUB 实测见 `fonts/provenance.txt` 的「第二件」一节。

| 字面 | 测试取的文件 | 字节数 | family / subfamily | `usWeightClass` | 字形数 |
|---|---|---|---|---|---|
| Regular | `NotoNaskhArabic/full/otf/NotoNaskhArabic-Regular.otf` | 156 216 | `Noto Naskh Arabic` / `Regular` | 400 | 1 719 |
| Bold | `NotoNaskhArabic/full/otf/NotoNaskhArabic-Bold.otf` | 158 768 | `Noto Naskh Arabic` / `Bold` | 700 | 1 719 |
| Medium | `NotoNaskhArabic/full/otf/NotoNaskhArabic-Medium.otf` | 181 912 | `Noto Naskh Arabic Medium` / `Regular` | 500 | 1 719 |
| SemiBold | `NotoNaskhArabic/full/otf/NotoNaskhArabic-SemiBold.otf` | 181 788 | `Noto Naskh Arabic SemiBold` / `Regular` | 600 | 1 719 |

- **zip 内件对平做过，且这次留了源 zip**：30 条 zip 条目对 30 件磁盘件，`两边都不缺`、`内容不同 0` ⇒ **30/30 逐字节一致**。上面 Noto Sans CJK SC 那件是「zip 未留、未做内件对平」，两份的可信度档位不同，引用时别混。
- **同字面多构建，不是重复件**：Regular 位 10 份不同哈希、Bold / Medium / SemiBold 各 5 份（`full` / `unhinted` / `hinted` / `googlefonts` × otf / ttf / variable / slim-variable）。全部登记，**测试只取上表 `full/otf/` 那份**；别按文件名当不同字面用。
- 合计 5 606 373 B = 5.35 MiB，最大单件 325 232 B（0.31 MiB），远低于 50 MiB 线，按「不开 LFS」口径以普通 blob 入库；许可 = SIL OFL 1.1（`OFL.txt` 随目录入库），版权行 `Copyright 2022 The Noto Project Authors`。
- **用途**：阿语（RTL）面的**第二份基准**——补第一份没有的脚本覆盖（Bidi 分趟 + 整形换形 + 覆盖率）。与第一份同级：它自己那套脚本的像素 golden 就拿它取；**但同一张 golden 不混两份字体**，各出各的脚本，否则两族 hinting 的差异归因不到改动。
- **判据为什么不是「字形数 < 码点数」**（实测出来的）：`full/otf/Regular` 的 GSUB 特性集含 `liga`，但 **`liga` 只有 2 条连字且不含 `lam + alef`，`rlig` 的 11 个 lookup 里 Type-4 连字 0 条** —— 这份字体把 `لا` 做成单字置换 `uni0644.init.rlig` + `uni0627.fina.rlig`，字数一点不减。拿数数当判据就是假红。
  改后的判据 = **`.notdef = 0`（确有阿语覆盖）且 shaped 字形号 ≠ nominal 字形号至少一处**。活体读数（HarfBuzz 14.4.0 + `hb-ot-font`）：`لا` nominal/shaped = `66/10`、`8/71`，生效 2 处 ⇒ PASS；同两个串在基准族上全部 `.notdef` ⇒ 报「不适用」而不是「整形失败」。

## 字幕表情符号取样字体：NotoColorEmoji（2026-10-24 入库）

来源：`https://github.com/googlefonts/noto-emoji/blob/main/2D/fonts/NotoColorEmoji.ttf`（仓库 `googlefonts/noto-emoji` · **分支 `main` 的当前对象，不是 release 件**）。
入库对象 = `fonts/noto-emoji/` 下 **1 份字体件 + 1 份 `LICENSE`**；身份、表清单、strike 与覆盖率实测见 `fonts/provenance.txt` 的「第三件」一节。

| 项 | 值 |
|---|---|
| 字节数 | 10 730 124 B（+ `LICENSE` 4 301 B = 10.24 MiB） |
| SHA-256 | `15671215…d6cc44`（完整值见 `provenance.txt`） |
| 上游对平 | git blob SHA1 `e605743d…f335` **与 `main` 树条目逐位相同**、大小同值 ⇒ 入库字节 = 仓库内件同一对象 |
| 内部版本 | `Version 2.057;GOOG;noto-emoji:20260911:fc4ca365…`（末尾令牌在公开仓库查不到提交，**不作来源锚点**） |
| 字形构成 | **纯位图**：`CBDT` / `CBLC`，**无 `glyf` / `CFF` / `COLR` / `CPAL` / `SVG`**；`numGlyphs = 4046`、`unitsPerEm = 2048` |
| 位图档 | **唯一一档 109 × 109 ppem** |
| 覆盖 | `cmap` 1 510 码位（emoji 区段 1207 + 116 + 旗帜 26）；**拉丁与 CJK 全部无覆盖** |
| 字重 / 样式 | 仅 `Regular`：`usWeightClass = 400`、`fsSelection = 0x0040`（无 ITALIC 位）、`italicAngle = 0` |
| 许可 | SIL OFL 1.1；`LICENSE` 与第一件的 `LICENSE` **逐字节相同**（OFL 正文本身，字体专属版权行在 `name` 表里） |

- **用途**：补回退链的 emoji 一档——没有它，那一档只能永远停在豆腐块。它**不是基准族**，与基准族脚本覆盖零重叠，故**不出共用 golden**；三份字体各出各的脚本面。
- **为什么这里先不写判据**：三条形态后果都还没实测——① 无轮廓 ⇒ `hb-ot-font` / `hb-ft` 两条度量入口在这件上走什么路径待验；② 单 strike ⇒ 非 109 px 字号必走位图缩放，**跨平台像素比对不成立**；③ 只有一档 Regular ⇒ 组合选脸的 `style` 硬过滤会直接拒斜体请求（这正好是「不准静默降级」那条的现成材料）。判据等表情面开工时一并拍，登记只立事实。

## 视频编解码素材怎么取用

外部示例视频与自写序列的来源、整件 SHA-256、取帧命令、色彩标记与对接事实都登记在各自的
`provenance.txt` 里（`fixtures/provenance.txt`、`synth/provenance.txt`）——**凡是产生数值的输入，
同目录必须有一份溯源登记**，缺条目的数值只算「跑出来过」，不算可复现。取帧一律走 rawvideo 管道，
不在仓库里留大体积 raw 文件。

## 生成与确定性

```bash
python scripts/gen_fixtures.py            # 默认写 assets/fixtures/
python scripts/gen_fixtures.py --out DIR  # 写到别处（比对用）
```

**确定性是硬要求**：字节流只能是输入的纯函数，不得含时间戳、路径、随机数或任何随环境变化的量。
入库前置条件 = 连跑两次产出逐字节一致（`diff -r` 无差异）。

生成器是**独立实现**（不复用 C++ 侧代码），所以它同时是一份交叉验证：Python 写、库的读方读，
两边对不上就说明有一方偏离了格式。核对方式：

```bash
demo_local/bin/clv_demo_container.exe info assets/fixtures/neg_dts.clv
```

## 冻结值（重跑必须复现，否则先查确定性再入库）

| 文件 | 字节数 | 整文件 CRC-32/MPEG-2 | SHA-256（前 16） |
|---|---|---|---|
| `minimal.clv` | 377 | `17655092` | `2bcdff83afd6f8d1` |
| `neg_dts.clv` | 1123 | `C07BC799` | `36107e6d4e9d3690` |
| `subtitle_ext4.clv` | 1776 | `D0AB5526` | `364399556f2ecd1f` |
| `subtitle_pipeline.clv` | 1388 | `5EF5FA73` | `f487da4fee675fba` |
| `demo_avs_360p_q32.clv` | 5 717 545 | `F1FBA5ED` | `81f82111758c1d59` |
| `demo_av_1080p_q32.clv` | 42 137 405 | `55130B49` | `8f177f554e1cfd07` |

**最后一行的复现路径与前三行不同**：它不由 `gen_fixtures.py` 产出，而由本库的编码 + 封装面产出，
配方与两条重跑对平（`enc` 的载荷仓同哈希、`mux` 的产物逐字节相同）见 `fixtures/provenance.txt` 同名一节。

**上一组值（容器格式版本 2 / 0 期、视频流尚无 `ext_type = 3` 参数块，2026-10-12 随「参数块对视频流必选」这一条落地而作废，留痕不抹行）**：
`minimal.clv` 347 B `62B12E84` / `4608ac2ea98303fd`、`neg_dts.clv` 1093 B `E5B63DA0` / `46a539bf2743514c`、
`subtitle_ext4.clv` 1746 B `DD39843E` / `8c7f85501dac31b3`。三件各增 30 B（每条视频流一个 30 B 的参数块：
前缀 18 + 载荷 8 + crc32 4；字幕流不写该块），其余字节一个没动。新值由 `scripts/gen_fixtures.py` 现算，
另用独立模型（自写 CRC-32/MPEG-2 逐位长除 + `hashlib`）逐件复算对平 **3 / 3**。

**上一组值（格式版本 1 / 0 期，2026-10-12 随容器格式版本升 2 作废，留痕不抹行）**：
`minimal.clv` `A941D1E9` / `64f4b2baab104026`、`neg_dts.clv` `6B24537C` / `00d47ac6b9d11303`、
`subtitle_ext4.clv` `6B68A197` / `b9b1ac43ea022b54`。字节数三件都不变（升版本只动文件头偏移 8 的两字节），
只有整件 CRC 与哈希跟着变。

**容器格式版本或结构口径变更必须重跑**：文件头带 `version_major` / `version_minor`（现 2 / 0）——**只有这两条轨道**动到才改这三件字节；工程版本（`0.0.x`）递增不触发重跑，它与容器格式版本之间没有换算关系。
此时**重跑 `scripts/gen_fixtures.py` 并更新本表**，别手改素材文件、别留着旧值。「格式变更须伴随版本升号」是义务条款，见 `README.md` 的版本口径节。
CRC-32/MPEG-2 参数集：poly `0x04C11DB7`、init `0xFFFFFFFF`、不反射、xorout 0（与 `src/core/crc32.h` 一致）。
