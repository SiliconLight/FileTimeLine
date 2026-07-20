#ifndef RAWPREVIEW_H
#define RAWPREVIEW_H

#include <QByteArray>
#include <QString>

// 从图片文件中提取内嵌预览图，避免整图解码（对 SMB/网络路径尤为重要）：
//  - 相机 RAW（TIFF 结构：ARW/CR2/NEF/DNG/ORF/RW2/RAF/PEF/SR2 等）
//    提取相机写入的内嵌 JPEG 预览段
//  - 相机 JPG：提取 EXIF（APP1）中的内嵌缩略图，只读文件头几十 KB
// 注：CR3（佳能 ISO-BMFF 结构）不在支持范围内。
namespace RawPreview {

// 判断是否为支持的 RAW 扩展名（小写）
bool isRawSuffix(const QString &suffix);

// 提取 RAW 中宽度最接近且不小于 targetWidth 的内嵌 JPEG 预览；
// 没有足够大的则取最大的一张。失败返回空 QByteArray。
QByteArray extractEmbeddedJpeg(const QString &path, int targetWidth);

// 提取 JPG 文件 EXIF 中的内嵌缩略图（只读文件头）。
// 无 EXIF 缩略图时返回空 QByteArray。
QByteArray extractExifThumbnail(const QString &path);

// 读取拍摄参数摘要（焦距/光圈/快门/ISO），如 "35mm  f/2.8  1/250s  ISO100"。
// 支持相机 RAW（TIFF 结构）与 JPG；其他格式或读取失败返回空串。
QString extractExifSummary(const QString &path);

} // namespace RawPreview

#endif // RAWPREVIEW_H
