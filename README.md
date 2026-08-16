# PDF Extractor

一个用 C++ 17 + Qt 5 编写的 PDF 解析与提取工具，同时提供 GUI 与命令行两种用法。

## 功能

### GUI（`pdfx` 双击或直接运行）

- 打开 PDF 后展示页面树、对象列表与元信息，支持深色 / 浅色主题切换
- 对象详情分为多个标签页：
  - **Structure（结构）**：以可折叠树展示对象引用关系
  - **Text（文本）**：展示解码后的流内容预览
  - **Hex（十六进制）**：流原始字节的十六进制转储
  - **Image（图像）**：渲染 DCTDecode（JPEG）、FlateDecode（PNG）等图像流
  - **Content（内容）**：内容流的原始操作符文本
  - **Fonts（字体）**：解析嵌入的 Type1C / CIDFontType0C（CFF）字体的字体名、字形数与字符串表；对普通内容流则抽取 `Tj` / `TJ` 运算符实际显示的文本
- 单页 / 页面树模式查看，支持缩放（放大、缩小、适应、原始尺寸）
- 导出全部图像或选中对象，右键菜单可保存原始流、预览文本与图像
- 最近打开文件记录（最多 10 条），界面语言可切换 中文 / English
- 支持拖放 PDF 文件打开

### 命令行（`pdfx <命令>`）

```
pdfx                       启动 GUI
pdfx <file.pdf>            在 GUI 中打开文件
pdfx list <file.pdf>       列出所有对象
pdfx info <file.pdf>       显示文件结构（xref / trailer）
pdfx preview <file.pdf> <id>   显示对象解码后的内容
pdfx extract <file.pdf> [id...]    提取流到输出目录
pdfx export <file.pdf> <id>       导出单个流（图像）
pdfx tree <file.pdf> <id>         显示对象的树结构
```

### 支持的过滤解码

- FlateDecode（含 Predictor）
- ASCII85Decode / ASCIIHexDecode
- RunLengthDecode
- LZWDecode
- DCTDecode / JPXDecode / JBIG2Decode 等作为透传字节保留

## 依赖

- CMake ≥ 3.16
- 支持 C++ 17 的编译器
- Qt 5（Widgets）
- zlib

## 构建

### Linux（Ubuntu / Debian）

```bash
sudo apt install qtbase5-dev zlib1g-dev cmake build-essential
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

### Windows

推荐使用 MSYS2 MinGW 环境：

```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-gcc mingw-w64-x86_64-qt5 mingw-w64-x86_64-zlib
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/mingw64
cmake --build build
```

### CI

GitHub Actions 已在 `.github/workflows/build.yml` 中配置，自动构建并出包
`linux-amd64` 与 `windows-amd64` 两个平台产物。

## 测试

生成测试 PDF 并做冒烟验证：

```bash
python3 tools/make_test_pdf.py
./build/pdfx list rich.pdf
./build/pdfx preview rich.pdf 3
```

另有 `tests/selftest.cpp` 可编译为 `pdfselftest`，对解析器做更细致的校验。

## 说明

- 配置文件位置：`~/.config/pdf-extractor/pdf-extractor.conf`（语言与最近文件）
- 本工具以解析与提取为主，并非完整的 PDF 渲染器；复杂资源仍可能无法完整还原