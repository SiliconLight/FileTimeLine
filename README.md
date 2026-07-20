# fileTimeline

一个 Qt 桌面小工具：遍历某个文件夹，把里面的文件按修改时间铺到一条**可缩放、可拖动的时间线**上，用时间轴的视角浏览文件（尤其是照片）。

A Qt desktop tool that lays the files of a folder out on a **zoomable, pannable timeline** by their modification time — browse your files (especially photos) along a time axis.

![screenshot](docs/screenshot.png)

## 功能 / Features

- 按修改时间把文件绘制在时间轴上，时间相同则按文件名排序；可选是否包含子目录
  Files are plotted by modification time (ties broken by file name); subdirectories optional
- 滚轮缩放（以鼠标为锚点）、左键拖拽平移，从同毫秒到跨数年任意级别查看
  Wheel-zoom anchored at the cursor, drag to pan — from single milliseconds to years
- 图片文件显示缩略图卡片，悬停弹出大图预览，预览大小三档可调
  Thumbnail cards for images, large hover preview with three selectable sizes
- 相机 **RAW**（ARW/CR2/NEF/DNG/ORF/RW2/RAF/PEF/SR2）直接提取内嵌预览图，不整读大文件，对 SMB/网络路径友好；相机 **JPG** 优先读 EXIF 内嵌缩略图
  Camera **RAW** files (ARW/CR2/NEF/DNG/ORF/RW2/RAF/PEF/SR2) use the embedded preview JPEG instead of decoding the whole file — friendly to SMB/network shares; camera **JPEGs** prefer the EXIF embedded thumbnail
- 悬停预览显示拍摄参数：焦距 / 光圈 / 快门 / ISO（从 EXIF 解析）
  Hover preview shows shooting parameters parsed from EXIF: focal length / aperture / shutter / ISO
- 双击文件用系统默认程序打开；非图片文件按扩展名着色
  Double-click to open a file with the system default app; non-image files are color-coded by extension

## 构建 / Build

环境：Qt 6.7（qmake）+ MinGW 64-bit，Windows。

Requires Qt 6.7 (qmake) and MinGW 64-bit on Windows.

```bash
qmake fileTimeline.pro
mingw32-make
```

或在 Qt Creator 中打开 `fileTimeline.pro` 直接构建。

Or simply open `fileTimeline.pro` in Qt Creator and build.

## 目录结构 / Layout

```
src/         源码 / source code
resources/   图标与 Qt 资源 / icons and Qt resources
docs/        文档与截图 / docs and screenshots
```

## 许可证 / License

[GPL-3.0](LICENSE)
