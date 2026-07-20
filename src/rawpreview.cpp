#include "rawpreview.h"

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSize>
#include <QStringList>

#include <algorithm>

namespace {

// TIFF 标签
constexpr quint16 kTagSubIfds    = 0x014A;
constexpr quint16 kTagJpegOffset = 0x0201; // JPEGInterchangeFormat
constexpr quint16 kTagJpegLength = 0x0202; // JPEGInterchangeFormatLength

struct Candidate { quint32 offset; quint32 length; };

// TIFF 解析器基类：数据源由子类提供（文件随机读 / 内存缓冲）
class TiffReader
{
public:
    virtual ~TiffReader() = default;
    virtual QByteArray read(qint64 off, qint64 len) = 0;

    bool parseHeader()
    {
        const QByteArray head = read(0, 8);
        if (head.size() < 8)
            return false;
        if (head.startsWith("II"))
            m_littleEndian = true;
        else if (head.startsWith("MM"))
            m_littleEndian = false;
        else
            return false;
        if (u16(2) != 42) // TIFF magic
            return false;
        m_ifd0 = u32(4);
        return m_ifd0 > 0;
    }

    quint16 u16(qint64 off)
    {
        const QByteArray b = read(off, 2);
        if (b.size() < 2)
            return 0;
        const auto *p = reinterpret_cast<const uchar*>(b.constData());
        return m_littleEndian ? quint16(p[0] | (p[1] << 8))
                              : quint16((p[0] << 8) | p[1]);
    }

    quint32 u32(qint64 off)
    {
        const QByteArray b = read(off, 4);
        if (b.size() < 4)
            return 0;
        const auto *p = reinterpret_cast<const uchar*>(b.constData());
        return m_littleEndian
            ? quint32(p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24))
            : quint32((quint32(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    }

    qint64 ifd0() const { return m_ifd0; }

    // 在指定 IFD 中查找单个标签的值（LONG/偏移型）
    quint32 findIfdTag(qint64 ifdOff, quint16 wanted)
    {
        if (ifdOff <= 0)
            return 0;
        const quint16 count = u16(ifdOff);
        if (count == 0 || count > 512)
            return 0;
        for (int i = 0; i < count; ++i) {
            const qint64 e = ifdOff + 2 + qint64(i) * 12;
            if (u16(e) == wanted)
                return u32(e + 8);
        }
        return 0;
    }

    // 读取 EXIF 子 IFD 中的光圈/快门/ISO/焦距（供悬停预览展示）
    struct ExifTags { double fnumber = 0.0; double exposure = 0.0; int iso = 0; double focal = 0.0; };

    void readExifTags(ExifTags &out)
    {
        constexpr quint16 kTagExifIfd  = 0x8769; // ExifIFDPointer
        constexpr quint16 kTagExposure = 0x829A; // ExposureTime, RATIONAL
        constexpr quint16 kTagFNumber  = 0x829D; // FNumber, RATIONAL
        constexpr quint16 kTagIso      = 0x8827; // ISOSpeedRatings, SHORT/LONG
        constexpr quint16 kTagFocal    = 0x920A; // FocalLength, RATIONAL（毫米）

        const qint64 exifIfd = findIfdTag(m_ifd0, kTagExifIfd);
        if (exifIfd <= 0)
            return;
        const quint16 count = u16(exifIfd);
        if (count == 0 || count > 512)
            return;
        for (int i = 0; i < count; ++i) {
            const qint64 e = exifIfd + 2 + qint64(i) * 12;
            switch (u16(e)) {
            case kTagExposure:
                out.exposure = rationalAt(e);
                break;
            case kTagFNumber:
                out.fnumber = rationalAt(e);
                break;
            case kTagFocal:
                out.focal = rationalAt(e);
                break;
            case kTagIso: {
                const quint16 type = u16(e + 2);
                if (type == 3)
                    out.iso = u16(e + 8);      // SHORT：值内联
                else if (type == 4)
                    out.iso = int(u32(e + 8)); // LONG
                break;
            }
            default:
                break;
            }
        }
    }

    // 递归扫描一个 IFD（含 SubIFD 与 next-IFD 链），收集所有 JPEG 预览段。
    // 只采信 JPEGInterchangeFormat 标签；StripOffsets 不可信——
    // 部分机型的 RAW 数据本身是 lossless-JPEG 压缩，会混淆其中。
    void scanIfd(qint64 off, int depth, QList<Candidate> &out, int &visited)
    {
        if (off <= 0 || depth > 4 || ++visited > 64)
            return;
        const quint16 count = u16(off);
        if (count == 0 || count > 512)
            return;
        const QByteArray entries = read(off + 2, qint64(count) * 12 + 4);
        if (entries.size() < qint64(count) * 12 + 4)
            return;

        quint32 jpegOff = 0, jpegLen = 0;
        QList<quint32> subIfds;

        for (int i = 0; i < count; ++i) {
            const qint64 e = off + 2 + qint64(i) * 12;
            const quint16 tag = u16(e);
            const quint32 num = u32(e + 4);

            switch (tag) {
            case kTagJpegOffset:
                jpegOff = u32(e + 8);
                break;
            case kTagJpegLength:
                jpegLen = u32(e + 8);
                break;
            case kTagSubIfds: // LONG 数组，值内联或按偏移
                for (quint32 k = 0; k < num; ++k)
                    subIfds.append(u32(num <= 1 ? e + 8 : u32(e + 8) + qint64(k) * 4));
                break;
            default:
                break;
            }
        }

        if (jpegOff && jpegLen)
            out.append({jpegOff, jpegLen});
        for (quint32 sub : subIfds)
            scanIfd(sub, depth + 1, out, visited);

        const quint32 next = u32(off + 2 + qint64(count) * 12);
        if (next)
            scanIfd(next, depth, out, visited); // 兄弟 IFD 链，不加深递归
    }

private:
    // 读取条目对应的 RATIONAL 值（type=5，count=1）；失败返回 0
    double rationalAt(qint64 entry)
    {
        if (u16(entry + 2) != 5)
            return 0.0;
        const qint64 off = u32(entry + 8);
        const quint32 num = u32(off), den = u32(off + 4);
        return den ? double(num) / double(den) : 0.0;
    }

    bool   m_littleEndian = true;
    qint64 m_ifd0 = 0;
};

// 文件随机读（RAW：只 seek 需要的区段，不整读大文件）
class TiffFileReader : public TiffReader
{
public:
    bool open(const QString &path)
    {
        m_file.setFileName(path);
        return m_file.open(QIODevice::ReadOnly) && parseHeader();
    }
    QByteArray read(qint64 off, qint64 len) override
    {
        if (!m_file.seek(off))
            return {};
        return m_file.read(len);
    }
private:
    QFile m_file;
};

// 内存缓冲（JPEG EXIF 段：数据已在内存中）
class TiffMemReader : public TiffReader
{
public:
    explicit TiffMemReader(QByteArray data) : m_data(std::move(data)) {}
    bool open() { return parseHeader(); }
    QByteArray read(qint64 off, qint64 len) override
    {
        if (off < 0 || off >= m_data.size())
            return {};
        return m_data.mid(int(off), int(qMin(len, m_data.size() - off)));
    }
private:
    QByteArray m_data;
};

// 校验段头：必须是标准 JPEG（SOI + 常规 marker），
// 排除 lossless JPEG（SOF3=0xC3 等，Qt 无法解码）
bool isStandardJpeg(const QByteArray &head)
{
    if (head.size() < 4 || uchar(head[0]) != 0xFF || uchar(head[1]) != 0xD8
        || uchar(head[2]) != 0xFF)
        return false;
    const uchar m = uchar(head[3]);
    return !(m == 0xC3 || m == 0xC5 || m == 0xC6 || m == 0xC7
             || m == 0xC9 || m == 0xCB || m == 0xCD || m == 0xCF);
}

// 从 JPEG 头部扫描 SOF marker 取得图像尺寸；失败返回空 QSize
QSize jpegSize(const QByteArray &head)
{
    if (!isStandardJpeg(head))
        return {};
    int pos = 2;
    while (pos + 9 < head.size()) {
        if (uchar(head[pos]) != 0xFF)
            return {};
        const uchar marker = uchar(head[pos + 1]);
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        const int segLen = (uchar(head[pos + 2]) << 8) | uchar(head[pos + 3]);
        if (segLen < 2)
            return {};
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4
            && marker != 0xC8 && marker != 0xCC) {
            const int h = (uchar(head[pos + 5]) << 8) | uchar(head[pos + 6]);
            const int w = (uchar(head[pos + 7]) << 8) | uchar(head[pos + 8]);
            return QSize(w, h);
        }
        pos += 2 + segLen;
    }
    return {};
}

// 读取 JPG 文件头中的 EXIF（APP1）TIFF 数据段；无则返回空
QByteArray jpegExifTiff(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = f.read(256 * 1024); // EXIF APP1 位于文件头
    if (head.size() < 4 || uchar(head[0]) != 0xFF || uchar(head[1]) != 0xD8)
        return {};

    int pos = 2;
    while (pos + 4 <= head.size()) {
        if (uchar(head[pos]) != 0xFF)
            break;
        const uchar marker = uchar(head[pos + 1]);
        if (marker == 0xDA) // SOS：图像数据开始，EXIF 只在前面
            break;
        const int segLen = (uchar(head[pos + 2]) << 8) | uchar(head[pos + 3]);
        if (segLen < 2 || pos + 2 + segLen > head.size())
            break;
        if (marker == 0xE1 && segLen >= 8
            && head.mid(pos + 4, 6) == QByteArray("Exif\0\0", 6))
            return head.mid(pos + 10, segLen - 8);
        pos += 2 + segLen;
    }
    return {};
}

} // namespace

bool RawPreview::isRawSuffix(const QString &s)
{
    static const char *raws[] = {"arw","cr2","nef","dng","orf","rw2","raf","pef","sr2"};
    for (const char *r : raws)
        if (s == QLatin1String(r))
            return true;
    return false;
}

QByteArray RawPreview::extractEmbeddedJpeg(const QString &path, int targetWidth)
{
    TiffFileReader tiff;
    if (!tiff.open(path))
        return {};

    QList<Candidate> candidates;
    int visited = 0;
    tiff.scanIfd(tiff.ifd0(), 0, candidates, visited);

    struct Scored { quint32 offset; quint32 length; QSize size; };
    QList<Scored> ok;
    for (const Candidate &c : candidates) {
        if (c.length < 512 || c.length > 200u * 1024 * 1024)
            continue;
        const QByteArray head = tiff.read(c.offset, 4096);
        if (!isStandardJpeg(head))
            continue;
        ok.append({c.offset, c.length, jpegSize(head)});
    }
    if (ok.isEmpty())
        return {};

    // 优先选宽度 >= targetWidth 中字节数最小的段（够用且最省 IO）
    int best = -1;
    for (int i = 0; i < ok.size(); ++i) {
        if (ok[i].size.width() >= targetWidth
            && (best < 0 || ok[i].length < ok[best].length))
            best = i;
    }
    // 都不够大则取实际宽度最大的一段
    if (best < 0) {
        best = 0;
        for (int i = 1; i < ok.size(); ++i)
            if (ok[i].size.width() > ok[best].size.width())
                best = i;
    }
    return tiff.read(ok[best].offset, ok[best].length);
}

QByteArray RawPreview::extractExifThumbnail(const QString &path)
{
    const QByteArray data = jpegExifTiff(path);
    if (data.isEmpty())
        return {};
    TiffMemReader tiff(data);
    if (!tiff.open())
        return {};
    QList<Candidate> candidates;
    int visited = 0;
    tiff.scanIfd(tiff.ifd0(), 0, candidates, visited);
    for (const Candidate &c : candidates) {
        const QByteArray jpg = tiff.read(c.offset, c.length);
        if (isStandardJpeg(jpg))
            return jpg;
    }
    return {};
}

QString RawPreview::extractExifSummary(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    TiffReader::ExifTags tags;

    if (isRawSuffix(suffix)) {
        TiffFileReader tiff; // RAW：TIFF 结构直接随机读，只取几个标签
        if (!tiff.open(path))
            return {};
        tiff.readExifTags(tags);
    } else if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        const QByteArray data = jpegExifTiff(path);
        if (data.isEmpty())
            return {};
        TiffMemReader tiff(data);
        if (!tiff.open())
            return {};
        tiff.readExifTags(tags);
    } else {
        return {};
    }

    QStringList parts;
    if (tags.focal > 0.0) {
        // 整数毫米显示 "35mm"，非整数（如 35.5）保留一位小数
        const double r = qRound64(tags.focal);
        if (qAbs(tags.focal - r) < 0.05)
            parts << QStringLiteral("%1mm").arg(r);
        else
            parts << QStringLiteral("%1mm").arg(tags.focal, 0, 'f', 1);
    }
    if (tags.fnumber > 0.0)
        parts << QStringLiteral("f/%1").arg(tags.fnumber, 0, 'f', 1);
    if (tags.exposure > 0.0) {
        if (tags.exposure >= 1.0) {
            QString s = QString::number(tags.exposure, 'f', 1);
            if (s.endsWith(QLatin1String(".0")))
                s.chop(2);
            parts << s + QLatin1Char('s');
        } else {
            parts << QStringLiteral("1/%1s").arg(qRound(1.0 / tags.exposure));
        }
    }
    if (tags.iso > 0)
        parts << QStringLiteral("ISO%1").arg(tags.iso);
    return parts.join(QStringLiteral("  "));
}
