# ab-decompressor

一个用于解压 UnityFS 格式 AssetBundle 的 C++ 工具。将压缩的 `.ab` 文件还原为未压缩状态，特别支持了《明日方舟》游戏特有的 LZ4 变体。

## 功能

* 支持标准 UnityFS 格式（LZMA, LZ4, LZ4HC, LZHAM）。
* 支持明日方舟特有的 `LZ4AK` 解密与解压。
* 自动重新构建未压缩的 UnityFS 文件头和索引表。
* `--transform` 模式：**不解压**，只把 `LZ4AK` 的字节置换还原成标准 LZ4，使原版 Unity 播放器能直接加载明日方舟的 AB。
* `--dir` 模式：递归批量处理整个目录（原地改写）。

## 依赖

* **LZ4**
* **LZMA (LzmaLib)**
* **LZHAM**
* **xmake** (构建工具)
* 支持 C++20 的编译器

## 编译

项目使用 `xmake` 进行构建：

```bash
xmake

```

## 使用

```bash
# 标准解压
lzham-ab-decompressor.exe --game std input.ab [output.ab]

# 明日方舟解压
lzham-ab-decompressor.exe --game arknights char_002_amiya.ab

# 明日方舟 -> 标准 AB（字节变换，不解压，体积不变）
lzham-ab-decompressor.exe --transform char_002_amiya.ab

# 整个 StreamingAssets/AB 目录原地转换（约 115 MB/s）
lzham-ab-decompressor.exe --transform --dir <dir>
```

### 参数说明

* `--game std`: 使用标准解压逻辑（默认）。
* `--game arknights`: 使用针对明日方舟修改的 LZ4 逻辑。
* `--transform`: 不解压，只做 LZ4AK -> 标准 LZ4 的字节置换（token 半字节互换、2 字节 match offset 交换），
  并把块的压缩 id 从 4 改成 2。数据长度完全不变，但原版 Unity（含自建播放器）从此可以直接
  `AssetBundle.LoadFromFile` —— 否则会报 `Decompressing this format (4) is not supported on this platform`。
* `--dir <dir>`: 递归处理目录下所有 `*.ab`，原地改写（先写 `.tmp` 再改名）。可与 `--transform` 组合。
* 若不指定输出路径，默认生成 `文件名_unpacked.ab`（`--transform` 时为 `文件名_std.ab`）。
