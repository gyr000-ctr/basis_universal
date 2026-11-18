# cTFBC2 (BC2/DXT3) transcode target — 记录 (2026-11-18)

## 需求
老移动管线只认 DXT3。新增 `cTFBC2_RGBA` 转码目标：色块按 BC1 规则（恒 4-color，同 BC3），
alpha 按 4-bit 显式存储（DXT3 布局：texel i 存于 64-bit alpha 块的 bit 4*i..4*i+3）。
要求：Punchthrough 语义正确；覆盖 alpha 全 0 / 全 1 / 非 4x4 尺寸边界；既有 BC1/BC3 输出逐字节不变。

## 改动 (8 files)

| 文件 | 改动 |
|---|---|
| `transcoder/basisu_transcoder.h` | 新增 `cTFBC2_RGBA = 41`（7 被 legacy `cTFBC7_ALT` 占用），`cTFTotalTextureFormats` 41→42，legacy 别名 `cTFBC2`；更新 DDS BC2 注释 |
| `transcoder/basisu_transcoder_internal.h` | `block_format` 末尾追加 `cBC2`（整 16 字节块）与 `cBC2_ALPHA`（仅 8 字节显式 alpha 块），不打乱既有枚举值 |
| `transcoder/basisu.h` | `texture_format` 末尾追加 `cBC2`（bytes/block=16、4x4 均走默认分支） |
| `transcoder/basisu_transcoder.cpp` | 核心：①`convert_etc1s_to_bc2_alpha()`（ETC1S 灰度→4-bit 显式 alpha，`(v*15+127)/255` 最近舍入，0→0、255→15 精确）；②ETC1S slice transcoder 增 `cBC2_ALPHA` case；③ETC1S `transcode_image` 增 `cTFBC2_RGBA`（alpha slice→`cBC2_ALPHA`，无 alpha→`write_opaque_alpha_blocks`，color→`cBC1` 且禁 3-color，与 BC3 完全同路径）；④`write_opaque_alpha_blocks` 增 `cBC2_ALPHA`（8×0xFF）；⑤`transcode_uastc_to_bc2()` + `write_bc2_alpha_block()`（UASTC 解块→显式 alpha + BC1 hint/encode，与 BC3 同色块路径）；⑥UASTC LDR slice/image transcoder 增 case；⑦`transcode_4x4_block`（XUASTC/XUBC7 共用）增 `cBC2`；⑧XUASTC/XUBC7 `transcode_image` 增 case；⑨`basis_is_format_supported` 三个列表（ETC1S 挂 `BASISD_SUPPORT_DXT1`、XUASTC/ASTC LDR、XUBC7）+ bytes-per-block/name/has_alpha/to-texture_format 表 |
| `transcoder/basisu_transcoder_uastc.h` | 声明 `transcode_uastc_to_bc2` |
| `encoder/basisu_gpu_texture.cpp` | KTX GL 常量 `0x83F2`/`0x8C4E`（DXT3 UNORM/sRGB）；`create_ktx_texture_file` internalFormat 分支（base=RGBA）；`does_ktx_support_format`、`ktx_gl_internal_format_name`、`get_texture_format_string`、`unpack_block`（复用已有 `bcu::unpack_bc2`） |
| `basisu_tool.cpp` | `-export_ktx` 格式表加 `"BC2"`，更新用法错误信息 |
| `basisu_tool_help.h` | `-export_ktx` 帮助文本加 BC2 |

关键语义：BC2 色块恒为 4-color（ETC1S 路径与 BC3 共用 `bc1_allow_threecolor_blocks=false`；
UASTC/XUASTC 路径 `encode_bc1` 本就不产 3-color 块），punchthrough 的 0/1 alpha 由显式
alpha 块精确承载（0→0x0、255→0xF），色块不再承担透明语义。

## 验证命令与结果

构建：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j12 basisu`（exit 0）

验收（<30s 要求，实测 0.09s）：
```
basisu -export_ktx BC2 -file alpha_grad64_etc1s.ktx2 -output_file out.ktx   # OK, 4164 bytes
basisu -ktx_info -file out.ktx
#   glInternalFormat : 0x8C4E (35918)  BC2 (DXT3 RGBA sRGB)   <- sRGB 源
# 线性源实测          : 0x83F2 (33778)  BC2 (DXT3 RGBA)
```

边界用例（ETC1S + UASTC 双源，Python 逐块校验，全部通过）：
- alpha 全 0：1024 个 nibble 全 0x0；alpha 全 1：全 0xF
- punchthrough 棋盘（0/255）：nibble 仅 {0,15} 且逐 texel 位置 0 失配
- 非 4x4 尺寸 13x7：4x2 块 = 128 字节；带 mipmap 13x7→4 级（13x7/6x3/3x1/1x1）尺寸链正确
- 无 alpha 切片的不透明源：alpha 块全 0xFF（opaque 路径）
- 所有输出文件所有块 color0 > color1（无 3-color/punchthrough 色块）
- 64x64 梯度：alpha 与源最大误差 11-12（≈4-bit 量化下限）；整图解码 color RMSE≈4（BC1 正常压缩误差）
- XUASTC（arith/zstd）与 ETC1S kodim 仓库文件均可导出

回归：10 个输入（kodim01/23、全部 alpha 用例、ETC1S+UASTC）× {BC1,BC3} 导出，
与改动前基线 **逐字节一致**（md5 全等）。ETC1S 源的 BC2 色块半部与 BC3 输出逐字节一致（同路径佐证）。

## 未做（后续）
- `-export_dds BC2`（DDS 写出侧）与 DDS 源→BC2 直通：不在本次范围
- `contrib/previewers` 单文件 transcoder 与 WASM/Python artifacts 需按仓库惯例在提交后重新生成
- JS 侧数值枚举无需改动（既有值未变；wrappers 用 `cTFTotalTextureFormats` 符号编译）
