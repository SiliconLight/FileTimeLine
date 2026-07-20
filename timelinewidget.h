#ifndef TIMELINEWIDGET_H
#define TIMELINEWIDGET_H

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QVector>
#include <QWidget>

// 单个文件的信息
struct FileItem {
    QString  name;      // 文件名
    QString  path;      // 完整路径
    QDateTime modified; // 最后修改时间
    qint64   size = 0;  // 字节数
    QString  suffix;    // 小写扩展名
};

// 自绘时间线控件：把一批文件按修改时间绘制在一条可缩放、可拖动的时间轴上。
// 交互：滚轮缩放（以鼠标位置为锚点）、左键拖拽平移、悬停显示文件详情。
// 缩略图模式下图片文件显示缩略图卡片，悬停时弹出大图预览。
class TimelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget *parent = nullptr);

    // 载入文件并按规则排序：修改时间升序；时间相同则名称升序（名称小的靠前）
    void setFiles(const QVector<FileItem> &files);
    const QVector<FileItem> &files() const { return m_files; }

    double msPerPixel() const { return m_msPerPixel; }
    double minMsPerPixel() const;
    double maxMsPerPixel() const;

    // 供后台加载线程回调（在主线程执行）
    void onThumbReady(const QString &path, int kind, const QImage &img, const QString &exif);

public slots:
    void zoomIn();                      // 以视图中心为锚点放大
    void zoomOut();                     // 以视图中心为锚点缩小
    void fitToContents();               // 缩放平移到刚好容纳全部文件
    void setMsPerPixel(double v);       // 以视图中心为锚点设置缩放级别
    void setShowThumbnails(bool on);    // 切换缩略图卡片模式
    void setPreviewSize(int px);        // 设置悬停预览图目标边长（切换后旧预览缓存失效）

signals:
    void zoomChanged(double msPerPixel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct NodeHit { QRect rect; int index; };  // 供悬停命中的节点区域

    void   zoomAt(double factor, double anchorX);
    double timeToX(qint64 ms) const { return (ms - m_viewStartMs) / m_msPerPixel; }
    int    nodeAt(const QPoint &pos) const;
    void   updateHover(const QPoint &pos);
    void   ensureThumb(const QString &path, int kind); // kind: 0=卡片缩略图 1=悬停预览
    void   drawHoverPreview(QPainter &p, const FileItem &f);

    static QColor  colorForSuffix(const QString &suffix);
    static bool    isImageSuffix(const QString &suffix);
    static QString sizeString(qint64 bytes);

    QVector<FileItem> m_files;                    // 已排序的文件列表
    QVector<NodeHit>  m_hits;                     // 上一帧绘制出的可命中区域
    double m_viewStartMs = 0.0;                   // 视口左缘对应的 epoch 毫秒
    double m_msPerPixel = 3600000.0;              // 缩放级别：每像素多少毫秒
    bool   m_panning = false;
    int    m_lastMouseX = 0;
    int    m_hoverIndex = -1;
    QPoint m_hoverPos;                            // 悬停位置（预览框定位用）

    bool m_showThumbs = true;                     // 缩略图卡片模式
    int  m_previewTarget = 400;                   // 悬停预览图目标边长
    QCache<QString, QPixmap> m_thumbs;            // 卡片缩略图缓存（LRU，按路径）
    QCache<QString, QPixmap> m_previews;          // 悬停大图缓存（LRU，按路径）
    QSet<QString> m_loading;                      // 正在加载的 "kind|path"
    QSet<QString> m_failed;                       // 加载失败的 "kind|path"
    QHash<QString, QString> m_exif;               // 拍摄参数摘要缓存（按路径）
};

#endif // TIMELINEWIDGET_H

