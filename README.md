# ab-decompressor

一个用于解压 UnityFS 格式 AssetBundle 的 C++ 工具。将压缩的 `.ab` 文件还原为未压缩状态，特别支持了《明日方舟》游戏特有的 LZ4 变体。

## 功能

* 支持标准 UnityFS 格式（LZMA, LZ4, LZ4HC, LZHAM）。
* 支持明日方舟特有的 `LZ4AK` 解密与解压。
* 自动重新构建未压缩的 UnityFS 文件头和索引表。

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
```

### 参数说明

* `--game std`: 使用标准解压逻辑（默认）。
* `--game arknights`: 使用针对明日方舟修改的 LZ4 逻辑。
* 若不指定输出路径，默认生成 `文件名_unpacked.ab`。
