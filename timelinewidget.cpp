#include "timelinewidget.h"
#include "rawpreview.h"

#include <QFileInfo>
#include <QDesktopServices>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QThreadPool>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// 常规整读路径：让解码器按目标尺寸降采样；超大文件跳过防止爆内存
static QImage readScaledImage(const QString &path, int target)
{
    if (QFileInfo(path).size() >= 300ll * 1024 * 1024)
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize orig = reader.size();
    if (orig.isValid()) {
        QSize s = orig;
        s.scale(target, target, Qt::KeepAspectRatio);
        reader.setScaledSize(s);
    }
    return reader.read();
}

// 后台缩略图加载任务：读图、缩放到目标尺寸，完成后回到主线程回调
class ThumbnailLoader : public QRunnable
{
public:
    ThumbnailLoader(TimelineWidget *widget, QString path, int kind, int previewTarget)
        : m_widget(widget), m_path(std::move(path)), m_kind(kind)
        , m_target(kind == 0 ? 96 : previewTarget) {}

    void run() override
    {
        const int target = m_target; // 卡片图 96 / 悬停预览图按当前档位
        const QString suffix = QFileInfo(m_path).suffix().toLower();
        QImage img;

        if (RawPreview::isRawSuffix(suffix)) {
            // 相机 RAW：按需提取尺寸合适的内嵌 JPEG 预览（不整读大文件）
            const QByteArray jpg = RawPreview::extractEmbeddedJpeg(m_path, target);
            if (!jpg.isEmpty())
                img = QImage::fromData(jpg, "JPG");
        } else if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
            // 相机 JPG：先试 EXIF 内嵌缩略图（只读文件头几十 KB，SMB 友好）
            const QByteArray jpg = RawPreview::extractExifThumbnail(m_path);
            if (!jpg.isEmpty()) {
                const QImage t = QImage::fromData(jpg, "JPG");
                // 悬停预览若 EXIF 缩略图太小则放弃，回退整读保证清晰度
                if (!t.isNull() && (m_kind == 0 || t.width() >= 256))
                    img = t;
            }
            if (img.isNull()) // 无 EXIF 缩略图（网络图等）或预览太小：回退整读
                img = readScaledImage(m_path, target);
        } else {
            img = readScaledImage(m_path, target);
        }

        if (!img.isNull() && (img.width() > target || img.height() > target))
            img = img.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        // 悬停预览顺带读拍摄参数（只读文件头几个标签，开销很小）
        QString exif;
        if (m_kind == 1)
            exif = RawPreview::extractExifSummary(m_path);

        TimelineWidget *w = m_widget;   // 作为 context：控件销毁后回调自动丢弃
        const QString path = m_path;
        const int kind = m_kind;
        QMetaObject::invokeMethod(w, [w, path, kind, img, exif]() { w->onThumbReady(path, kind, img, exif); },
                                  Qt::QueuedConnection);
    }

private:
    TimelineWidget *m_widget;
    QString m_path;
    int m_kind;
    int m_target;
};

namespace {
// 缩放范围（毫秒/像素）
constexpr double kMinMsPerPixel = 0.001;   // 最细可看清同一毫秒内的事件
constexpr double kMaxMsPerPixel = 1.0e10;  // 兜底上限，实际取动态上限

// 刻度“好看”间隔（毫秒），逐级递进
const double kTickSteps[] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500,
    1e3, 2e3, 5e3, 10e3, 15e3, 30e3,
    60e3, 120e3, 300e3, 600e3, 900e3, 1800e3,
    3600e3, 7200e3, 10800e3, 21600e3, 43200e3,
    86400e3, 172800e3, 604800e3, 1209600e3, 2592000e3, 7776000e3, 31536000e3, 157680000e3
};

// 主题色（明亮轻快感）
const QColor kBgTop      {"#ffffff"};
const QColor kBgBottom   {"#eef2fa"};
const QColor kAxisColor  {"#aab3cc"};
const QColor kTickText   {"#9aa1b8"};
const QColor kLabelText  {"#4a5170"};
const QColor kHintText   {"#a8aec2"};
const QColor kAccent     {"#6c6cf2"};
const qreal  kCardRadius = 12.0;

// 缩略图卡片尺寸
const int kIconSize = 48;   // 图标/缩略图边长
const int kCardH    = 78;   // 卡片高度（图标 + 文件名 + 间距）
const int kCardLaneH = 94;  // 卡片模式的层间距

// 图片扩展名（可显示缩略图；RAW 走内嵌预览提取）
const char *kImageExts[] = {"jpg","jpeg","png","gif","bmp","svg","webp","ico","tif","tiff",
                            "arw","cr2","nef","dng","orf","rw2","raf","pef","sr2"};
} // namespace

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::OpenHandCursor);

    m_thumbs.setMaxCost(10000);  // 96px 缩略图约 36KB/张，10000 张 ≈ 360MB
    m_previews.setMaxCost(100);  // 320px 预览约 400KB/张，100 张 ≈ 40MB
}

double TimelineWidget::minMsPerPixel() const
{
    return kMinMsPerPixel;
}

double TimelineWidget::maxMsPerPixel() const
{
    // 动态上限：缩到最粗也至少能看到约 1/3 的总跨度，避免缩没影
    double span = 3600e3;
    if (m_files.size() >= 2) {
        span = m_files.last().modified.toMSecsSinceEpoch()
             - m_files.first().modified.toMSecsSinceEpoch();
        span = qMax(span, 1000.0);
    }
    return qMin(kMaxMsPerPixel, qMax(3600e3, span * 3.0 / qMax(1, width())));
}

void TimelineWidget::setFiles(const QVector<FileItem> &files)
{
    m_files = files;
    std::sort(m_files.begin(), m_files.end(), [](const FileItem &a, const FileItem &b) {
        if (a.modified != b.modified)
            return a.modified < b.modified;
        return a.name < b.name; // 时间相同按名称升序，名称小的靠前
    });
    m_hoverIndex = -1;
    m_hits.clear();
    fitToContents();
}

void TimelineWidget::zoomAt(double factor, double anchorX)
{
    const double anchorTime = m_viewStartMs + anchorX * m_msPerPixel;
    m_msPerPixel = qBound(minMsPerPixel(), m_msPerPixel * factor, maxMsPerPixel());
    m_viewStartMs = anchorTime - anchorX * m_msPerPixel;
    update();
    emit zoomChanged(m_msPerPixel);
}

void TimelineWidget::zoomIn()  { zoomAt(0.7, width() / 2.0); }
void TimelineWidget::zoomOut() { zoomAt(1.4, width() / 2.0); }

void TimelineWidget::setMsPerPixel(double v)
{
    const double centerTime = m_viewStartMs + width() / 2.0 * m_msPerPixel;
    m_msPerPixel = qBound(minMsPerPixel(), v, maxMsPerPixel());
    m_viewStartMs = centerTime - width() / 2.0 * m_msPerPixel;
    update();
    emit zoomChanged(m_msPerPixel);
}

void TimelineWidget::fitToContents()
{
    if (m_files.isEmpty() || width() <= 0) {
        update();
        return;
    }
    const double first = m_files.first().modified.toMSecsSinceEpoch();
    const double last  = m_files.last().modified.toMSecsSinceEpoch();
    double span = last - first;
    if (span <= 0)
        span = 60000.0; // 所有文件同刻，给一分钟的展示窗口
    m_msPerPixel = qBound(minMsPerPixel(), span * 1.15 / width(), maxMsPerPixel());
    m_viewStartMs = first - span * 0.075;
    update();
    emit zoomChanged(m_msPerPixel);
}

QColor TimelineWidget::colorForSuffix(const QString &s)
{
    static const char *code[]  = {"c","cc","cpp","cxx","h","hpp","py","js","ts","java","cs","go","rs","html","css","json","xml","ui","pro","cmake","qml","ini","sh","bat"};
    static const char *doc[]   = {"txt","md","doc","docx","pdf","xls","xlsx","ppt","pptx","csv","rtf","log"};
    static const char *media[] = {"mp3","wav","flac","ogg","mp4","mkv","avi","mov","wmv","flv","m4a"};
    static const char *arch[]  = {"zip","rar","7z","tar","gz","bz2","xz","iso"};
    static const char *exec[]  = {"exe","dll","msi","apk","com","sys","so","dylib"};

    auto in = [&](const char *list[], int n) {
        for (int i = 0; i < n; ++i)
            if (s == QLatin1String(list[i]))
                return true;
        return false;
    };
    if (in(code,  int(sizeof(code)/sizeof(char*))))  return {"#66bb6a"}; // 绿
    if (in(doc,   int(sizeof(doc)/sizeof(char*))))   return {"#42a5f5"}; // 蓝
    if (isImageSuffix(s))                            return {"#ffa726"}; // 橙黄
    if (in(media, int(sizeof(media)/sizeof(char*)))) return {"#ab47bc"}; // 紫
    if (in(arch,  int(sizeof(arch)/sizeof(char*))))  return {"#ff8a65"}; // 珊瑚橙
    if (in(exec,  int(sizeof(exec)/sizeof(char*))))  return {"#ef5350"}; // 红
    return {"#26c6da"};                                                  // 青
}

bool TimelineWidget::isImageSuffix(const QString &s)
{
    for (const char *e : kImageExts)
        if (s == QLatin1String(e))
            return true;
    return false;
}

QString TimelineWidget::sizeString(qint64 bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QString::number(bytes) + " B"
                  : QString::number(v, 'f', 1) + " " + units[u];
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 背景：圆角卡片 + 细边框
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 0));
    p.drawRect(rect());
    QPainterPath cardClip;
    cardClip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            kCardRadius, kCardRadius);
    p.setClipPath(cardClip);

    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0, kBgTop);
    bg.setColorAt(1, kBgBottom);
    p.fillRect(rect(), bg);

    p.setClipping(false);
    p.setPen(QPen(QColor("#e4e8f2"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                      kCardRadius, kCardRadius);
    p.setClipPath(cardClip);

    if (m_files.isEmpty()) {
        p.setPen(kHintText);
        QFont f = p.font();
        f.setPointSize(13);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("点击左上角「选择目录」，把一个文件夹铺到时间线上"));
        return;
    }

    const int axisY = height() * 3 / 5;                 // 时间轴纵向位置（略偏下）
    const qint64 viewStart = qint64(m_viewStartMs);
    const qint64 viewEnd   = qint64(m_viewStartMs + width() * m_msPerPixel);

    // ---------- 网格与时间刻度 ----------
    double interval = kTickSteps[sizeof(kTickSteps)/sizeof(double) - 1];
    for (double s : kTickSteps) {
        if (s >= m_msPerPixel * 110.0) { interval = s; break; }
    }

    QString tickFmt;
    if (interval < 1e3)       tickFmt = "HH:mm:ss.zzz";
    else if (interval < 60e3) tickFmt = "HH:mm:ss";
    else if (interval < 86400e3) tickFmt = "MM-dd HH:mm";
    else                      tickFmt = "yyyy-MM-dd";

    auto drawTick = [&](qint64 t) {
        const double x = timeToX(t);
        p.setPen(QPen(QColor(90, 100, 140, 18), 1));
        p.drawLine(QPointF(x, 10), QPointF(x, height() - 34));
        p.setPen(QPen(kAxisColor.lighter(130), 1));
        p.drawLine(QPointF(x, axisY - 4), QPointF(x, axisY + 4));
        p.setPen(kTickText);
        p.drawText(QRectF(x - 80, axisY + 8, 160, 20), Qt::AlignHCenter,
                   QDateTime::fromMSecsSinceEpoch(t).toString(tickFmt));
    };

    if (interval >= 86400e3) {
        // 天级以上按本地日期零点对齐
        const int stepDays = qMax(1, int(std::llround(interval / 86400e3)));
        QDateTime cur = QDateTime::fromMSecsSinceEpoch(viewStart).date().startOfDay();
        while (cur.toMSecsSinceEpoch() <= viewEnd) {
            if (cur.toMSecsSinceEpoch() >= viewStart)
                drawTick(cur.toMSecsSinceEpoch());
            cur = cur.addDays(stepDays);
        }
    } else {
        const qint64 step = qint64(interval);
        for (qint64 t = viewStart / step * step; t <= viewEnd; t += step)
            drawTick(t);
    }

    // ---------- 时间轴 ----------
    p.setPen(QPen(kAxisColor, 2));
    p.drawLine(0, axisY, width(), axisY);

    // “现在”标记线
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs >= viewStart && nowMs <= viewEnd) {
        const double x = timeToX(nowMs);
        p.setPen(QPen(QColor(239, 83, 80, 170), 1, Qt::DashLine));
        p.drawLine(QPointF(x, 10), QPointF(x, height() - 34));
        p.setPen(QColor(239, 83, 80, 220));
        p.drawText(QRectF(x + 4, 12, 60, 18), QStringLiteral("现在"));
    }

    // ---------- 可见文件范围（二分查找） ----------
    const qint64 marginMs = qint64(220.0 * m_msPerPixel); // 标签可能超出点的两侧
    auto firstIt = std::lower_bound(m_files.begin(), m_files.end(), viewStart - marginMs,
        [](const FileItem &f, qint64 v) { return f.modified.toMSecsSinceEpoch() < v; });
    auto lastIt = std::upper_bound(m_files.begin(), m_files.end(), viewEnd + marginMs,
        [](qint64 v, const FileItem &f) { return v < f.modified.toMSecsSinceEpoch(); });

    // ---------- 标签分层布局：上下交替，同层不重叠 ----------
    const int laneH   = m_showThumbs ? kCardLaneH : 32;
    const int maxUp   = qMax(1, (axisY - 40) / laneH);
    const int maxDown = qMax(1, (height() - axisY - 44) / laneH);
    QVector<double> upRight(maxUp, -1e18), downRight(maxDown, -1e18);

    struct Node { int index; double x; int lane; bool upper; bool withLabel; };
    QVector<Node> nodes;
    nodes.reserve(lastIt - firstIt);

    const QFontMetrics fm = p.fontMetrics();
    const QFont baseFont = p.font();
    for (auto it = firstIt; it != lastIt; ++it) {
        const int idx = int(it - m_files.begin());
        const double x = timeToX(it->modified.toMSecsSinceEpoch());
        const int nodeW = m_showThumbs
            ? qBound(72, fm.horizontalAdvance(it->name) + 16, 120) // 卡片：容纳图标与文件名
            : fm.horizontalAdvance(it->name) + 20;                 // 经典：圆点 + 间距
        const double left = x - nodeW / 2.0, right = x + nodeW / 2.0;

        int lane = -1;
        bool upper = true;
        for (int l = 0; l < qMax(maxUp, maxDown); ++l) {
            if (l < maxUp && upRight[l] + 10 <= left) {
                lane = l; upper = true; upRight[l] = right; break;
            }
            if (l < maxDown && downRight[l] + 10 <= left) {
                lane = l; upper = false; downRight[l] = right; break;
            }
        }
        nodes.push_back({idx, x, lane, upper, lane >= 0});
    }

    // ---------- 绘制节点 ----------
    m_hits.clear();
    m_hits.reserve(nodes.size());
    for (const Node &n : nodes) {
        const FileItem &f = m_files[n.index];
        const QColor c = colorForSuffix(f.suffix);
        const bool hovered = (n.index == m_hoverIndex);

        if (!n.withLabel) {
            // lane 已满（文件过密）：仅画轴上时间点，不画卡片/文字、不请求缩略图，
            // 避免成百上千个节点挤在一起拖垮绘制与加载
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 200));
            p.drawEllipse(QPointF(n.x, axisY), 2.5, 2.5);
            continue;
        }

        if (m_showThumbs) {
            // ===== 缩略图卡片模式 =====
            const int cardW = qBound(72, fm.horizontalAdvance(f.name) + 16, 120);
            const int cardY = n.upper ? axisY - 14 - n.lane * laneH - kCardH
                                      : axisY + 14 + n.lane * laneH;
            const QRect card(int(n.x - cardW / 2), cardY, cardW, kCardH);

            // 连接线 + 轴上时间点
            p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), hovered ? 220 : 110), 1));
            p.drawLine(QPointF(n.x, axisY + (n.upper ? -5 : 5)),
                       QPointF(n.x, n.upper ? card.bottom() : card.top()));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 220));
            p.drawEllipse(QPointF(n.x, axisY), 2.5, 2.5);

            // 卡片底
            p.setPen(QPen(hovered ? kAccent : QColor("#e4e8f2"), hovered ? 1.6 : 1.0));
            p.setBrush(QColor(255, 255, 255, hovered ? 255 : 235));
            p.drawRoundedRect(card, 8, 8);

            // 图标区：图片显示缩略图，其余显示类型色块 + 后缀
            const QRect icon(card.center().x() - kIconSize / 2, card.top() + 5,
                             kIconSize, kIconSize);
            const QString failKey = QStringLiteral("0|") + f.path;
            const QPixmap *pm = nullptr;
            if (isImageSuffix(f.suffix) && !m_failed.contains(failKey))
                pm = m_thumbs.object(f.path); // 命中缓存即取，未命中不阻塞
            if (pm) {
                QPainterPath clip;
                clip.addRoundedRect(QRectF(icon), 6, 6);
                p.save();
                p.setClipPath(clip);
                QSize ts = pm->size();
                ts.scale(icon.size(), Qt::KeepAspectRatioByExpanding); // 居中裁剪填满
                p.drawPixmap(QRect(icon.center().x() - ts.width() / 2,
                                   icon.center().y() - ts.height() / 2,
                                   ts.width(), ts.height()), *pm);
                p.restore();
            } else if (isImageSuffix(f.suffix) && !m_failed.contains(failKey)) {
                ensureThumb(f.path, 0); // 异步加载，完成后自动重绘
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(c.red(), c.green(), c.blue(), 28));
                p.drawRoundedRect(icon, 6, 6);
                p.setBrush(c);
                p.drawEllipse(QPointF(icon.center()), 3.0, 3.0);
            } else {
                p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 120), 1));
                p.setBrush(QColor(c.red(), c.green(), c.blue(), 26));
                p.drawRoundedRect(icon, 6, 6);
                QString tag = f.suffix.isEmpty() ? QStringLiteral("?") : f.suffix.toUpper();
                if (tag.length() > 4)
                    tag = tag.left(4);
                QFont bf = baseFont;
                bf.setBold(true);
                if (tag.length() >= 4)
                    bf.setPointSizeF(qMax(7.0, bf.pointSizeF() - 2.5));
                p.setFont(bf);
                p.setPen(c.darker(115));
                p.drawText(icon, Qt::AlignCenter, tag);
                p.setFont(baseFont);
            }

            // 文件名（过长省略中间）
            p.setPen(hovered ? QColor("#2e3350") : kLabelText);
            p.drawText(QRect(card.left() + 5, card.bottom() - 21, cardW - 10, 16),
                       Qt::AlignHCenter, fm.elidedText(f.name, Qt::ElideMiddle, cardW - 10));

            m_hits.push_back({card, n.index});
        } else {
            // ===== 经典模式（圆点 + 文件名） =====
            const double laneY = n.upper ? axisY - 26.0 - n.lane * laneH
                                         : axisY + 26.0 + n.lane * laneH;

            // 连接线
            p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), hovered ? 220 : 110), 1));
            p.drawLine(QPointF(n.x, axisY + (n.upper ? -5 : 5)),
                       QPointF(n.x, laneY + (n.upper ? 8 : -8)));
            // 轴上时间点
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 220));
            p.drawEllipse(QPointF(n.x, axisY), 2.5, 2.5);

            // 标签节点（圆点 + 文件名）
            const int nodeW = fm.horizontalAdvance(f.name) + 20;
            const QRect nodeRect(int(n.x - nodeW / 2.0), int(laneY - 11), nodeW, 22);
            if (hovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(108, 108, 242, 26));
                p.drawRoundedRect(nodeRect.adjusted(-3, -2, 3, 2), 6, 6);
                p.setBrush(QColor(c.red(), c.green(), c.blue(), 60));
                p.drawEllipse(QPointF(n.x, laneY), 8.5, 8.5);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(n.x, laneY), hovered ? 5.0 : 4.0, hovered ? 5.0 : 4.0);
            p.setPen(hovered ? QColor("#2e3350") : kLabelText);
            p.drawText(nodeRect.adjusted(12, 0, 0, 0), Qt::AlignVCenter, f.name);

            m_hits.push_back({nodeRect.adjusted(-4, -4, 4, 4), n.index});
        }
    }

    // ---------- 悬停大图预览（缩略图模式 + 图片文件） ----------
    if (m_showThumbs && m_hoverIndex >= 0 && m_hoverIndex < m_files.size())
        drawHoverPreview(p, m_files[m_hoverIndex]);

    // ---------- 左上角：当前可视范围 ----------
    p.setPen(kHintText);
    p.drawText(QRect(14, 12, width() - 28, 20), Qt::AlignLeft,
               QStringLiteral("%1  ~  %2")
                   .arg(QDateTime::fromMSecsSinceEpoch(viewStart).toString("yyyy-MM-dd HH:mm:ss"),
                        QDateTime::fromMSecsSinceEpoch(viewEnd).toString("yyyy-MM-dd HH:mm:ss")));
}

int TimelineWidget::nodeAt(const QPoint &pos) const
{
    // 反向遍历：后绘制的节点在视觉上层，重叠时应优先命中
    for (int i = m_hits.size() - 1; i >= 0; --i)
        if (m_hits[i].rect.contains(pos))
            return m_hits[i].index;
    return -1;
}

void TimelineWidget::updateHover(const QPoint &pos)
{
    const int idx = nodeAt(pos);
    m_hoverPos = pos;
    if (idx == m_hoverIndex) {
        if (idx >= 0 && m_showThumbs)
            update(); // 预览框跟随鼠标移动
        return;
    }
    m_hoverIndex = idx;
    if (idx >= 0) {
        const FileItem &f = m_files[idx];
        if (m_showThumbs) {
            QToolTip::hideText(); // 缩略图模式下详情由悬停预览框展示
        } else {
            QToolTip::showText(mapToGlobal(pos),
                QStringLiteral("<b>%1</b><br/>"
                               "<span style='color:#8a90a8'>%2</span><br/>"
                               "修改时间：%3<br/>大小：%4")
                    .arg(f.name.toHtmlEscaped(), f.path.toHtmlEscaped(),
                         f.modified.toString("yyyy-MM-dd HH:mm:ss.zzz"), sizeString(f.size)),
                this);
        }
    } else {
        QToolTip::hideText();
    }
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent *event)
{
    zoomAt(event->angleDelta().y() > 0 ? 0.8 : 1.25, event->position().x());
    event->accept();
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_panning = true;
        m_lastMouseX = event->pos().x();
        setCursor(Qt::ClosedHandCursor);
        QToolTip::hideText();
    }
    QWidget::mousePressEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const int dx = event->pos().x() - m_lastMouseX;
        m_viewStartMs -= dx * m_msPerPixel;
        m_lastMouseX = event->pos().x();
        update();
    } else {
        updateHover(event->pos());
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        setCursor(Qt::OpenHandCursor);
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = nodeAt(event->pos());
        if (idx >= 0 && idx < m_files.size()) {
            m_panning = false; // 双击前会先触发 press，取消本次拖动状态
            setCursor(Qt::OpenHandCursor);
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_files[idx].path));
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineWidget::leaveEvent(QEvent *event)
{
    m_hoverIndex = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

void TimelineWidget::setShowThumbnails(bool on)
{
    m_showThumbs = on;
    m_hoverIndex = -1;
    m_hits.clear();
    update();
}

void TimelineWidget::setPreviewSize(int px)
{
    if (px == m_previewTarget)
        return;
    m_previewTarget = px;
    m_previews.clear(); // 旧尺寸的预览缓存作废
    // 预览类的失败标记也清掉，允许按新尺寸重试
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        if (it->startsWith(QLatin1String("1|")))
            it = m_failed.erase(it);
        else
            ++it;
    }
    update();
}

void TimelineWidget::ensureThumb(const QString &path, int kind)
{
    const QString key = QString::number(kind) + QLatin1Char('|') + path;
    if (m_loading.contains(key) || m_failed.contains(key))
        return;
    if ((kind == 0 && m_thumbs.contains(path)) || (kind == 1 && m_previews.contains(path)))
        return;
    m_loading.insert(key);
    QThreadPool::globalInstance()->start(new ThumbnailLoader(this, path, kind, m_previewTarget));
}

void TimelineWidget::onThumbReady(const QString &path, int kind, const QImage &img, const QString &exif)
{
    m_loading.remove(QString::number(kind) + QLatin1Char('|') + path);
    if (img.isNull()) {
        m_failed.insert(QString::number(kind) + QLatin1Char('|') + path);
    } else if (kind == 0) {
        m_thumbs.insert(path, new QPixmap(QPixmap::fromImage(img)), 1);
    } else {
        m_previews.insert(path, new QPixmap(QPixmap::fromImage(img)), 1);
    }
    if (!exif.isEmpty())
        m_exif.insert(path, exif);
    update();
}

void TimelineWidget::drawHoverPreview(QPainter &p, const FileItem &f)
{
    if (!isImageSuffix(f.suffix) || m_failed.contains(QStringLiteral("1|") + f.path))
        return;

    const QPixmap *pv = m_previews.object(f.path);
    if (!pv)
        ensureThumb(f.path, 1); // 先画占位框，加载完成后重绘出图

    const QFontMetrics fm = p.fontMetrics();
    const QSize imgSize = pv ? pv->size() : QSize(200, 130);
    const QString exif = m_exif.value(f.path);
    const int rowsH = exif.isEmpty() ? 40 : 58; // 有拍摄参数时多一行
    const int boxW = imgSize.width() + 16;
    const int boxH = imgSize.height() + 16 + rowsH; // 图 + 文件名/参数/时间

    // 默认出现在鼠标右下，越界则翻转到另一侧
    int x = m_hoverPos.x() + 18;
    int y = m_hoverPos.y() + 18;
    if (x + boxW > width() - 8)
        x = m_hoverPos.x() - boxW - 12;
    if (y + boxH > height() - 8)
        y = m_hoverPos.y() - boxH - 12;
    x = qBound(4, x, qMax(4, width() - boxW - 4));
    y = qBound(4, y, qMax(4, height() - boxH - 4));

    const QRect box(x, y, boxW, boxH);
    p.setPen(QPen(QColor("#d9deeb"), 1));
    p.setBrush(QColor(255, 255, 255, 250));
    p.drawRoundedRect(box, 8, 8);

    if (!pv) {
        p.setPen(kHintText);
        p.drawText(QRect(x + 8, y + 8, imgSize.width(), imgSize.height()),
                   Qt::AlignCenter, QStringLiteral("加载中…"));
    } else {
        p.drawPixmap(QRect(x + 8, y + 8, imgSize.width(), imgSize.height()), *pv);
    }
    p.setPen(kLabelText);
    p.drawText(QRect(x + 8, box.bottom() - rowsH, boxW - 16, 18), Qt::AlignHCenter,
               fm.elidedText(f.name, Qt::ElideMiddle, boxW - 24));
    if (!exif.isEmpty()) {
        p.setPen(kAccent);
        p.drawText(QRect(x + 8, box.bottom() - 40, boxW - 16, 16), Qt::AlignHCenter, exif);
    }
    p.setPen(kHintText);
    p.drawText(QRect(x + 8, box.bottom() - 22, boxW - 16, 16), Qt::AlignHCenter,
               QStringLiteral("%1 · %2").arg(f.modified.toString("yyyy-MM-dd HH:mm:ss"),
                                             sizeString(f.size)));
}
