#include "mainwindow.h"
#include "timelinewidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>

#include <cmath>

namespace {
// 缩放滑块的对数映射范围（毫秒/像素）
constexpr double kSliderMinZoom = 1e-3;   // log10 = -3
constexpr double kSliderMaxZoom = 1e10;   // log10 = 10
constexpr int    kSliderSteps   = 1000;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("文件时间线"));
    resize(1200, 680);

    // ---------- 顶部工具条 ----------
    auto *bar = new QWidget;
    bar->setObjectName("topBar");
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(14, 10, 14, 10);
    barLayout->setSpacing(10);

    auto *openBtn = new QPushButton(QStringLiteral("选择目录"));
    openBtn->setObjectName("primaryBtn");

    m_pathLabel = new QLabel(QStringLiteral("未选择目录"));
    m_pathLabel->setObjectName("pathLabel");
    m_pathLabel->setMinimumWidth(160);
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_recursiveBox = new QCheckBox(QStringLiteral("包含子目录"));
    m_recursiveBox->setChecked(false);

    auto *thumbBox = new QCheckBox(QStringLiteral("缩略图"));
    thumbBox->setChecked(true);
    thumbBox->setToolTip(QStringLiteral("图片文件在时间线上显示缩略图，悬停可看大图"));

    auto *previewCombo = new QComboBox;
    previewCombo->addItem(QStringLiteral("小预览"), 240);
    previewCombo->addItem(QStringLiteral("中预览"), 400);
    previewCombo->addItem(QStringLiteral("大预览"), 560);
    previewCombo->setCurrentIndex(1); // 默认中号
    previewCombo->setToolTip(QStringLiteral("悬停预览图的大小"));

    auto *zoomOutBtn = new QPushButton(QStringLiteral("−"));
    auto *zoomInBtn  = new QPushButton(QStringLiteral("＋"));
    zoomOutBtn->setFixedWidth(36);
    zoomInBtn->setFixedWidth(36);
    zoomOutBtn->setToolTip(QStringLiteral("缩小时间线"));
    zoomInBtn->setToolTip(QStringLiteral("放大时间线"));

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(0, kSliderSteps);
    m_zoomSlider->setFixedWidth(150);
    m_zoomSlider->setFixedHeight(22); // 给手柄圆点留足高度，避免上下被裁剪
    m_zoomSlider->setToolTip(QStringLiteral("缩放"));

    auto *fitBtn = new QPushButton(QStringLiteral("适应全部"));
    fitBtn->setToolTip(QStringLiteral("缩放平移到刚好显示所有文件"));

    m_countLabel = new QLabel;
    m_countLabel->setObjectName("countLabel");

    barLayout->addWidget(openBtn);
    barLayout->addWidget(m_pathLabel, 1);
    barLayout->addWidget(m_recursiveBox);
    barLayout->addWidget(thumbBox);
    barLayout->addWidget(previewCombo);
    barLayout->addSpacing(8);
    barLayout->addWidget(zoomOutBtn);
    barLayout->addWidget(m_zoomSlider);
    barLayout->addWidget(zoomInBtn);
    barLayout->addWidget(fitBtn);
    barLayout->addSpacing(8);
    barLayout->addWidget(m_countLabel);

    // ---------- 中央时间线 ----------
    m_timeline = new TimelineWidget;

    auto *central = new QWidget;
    central->setObjectName("centralArea");
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(bar);
    layout->addWidget(m_timeline, 1);
    setCentralWidget(central);

    statusBar()->showMessage(QStringLiteral("就绪 —— 滚轮缩放，左键拖拽平移，悬停查看文件详情"));

    // ---------- 信号连接 ----------
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::chooseDirectory);
    connect(m_recursiveBox, &QCheckBox::toggled, this, [this] {
        if (!m_currentDir.isEmpty())
            loadDirectory(m_currentDir);
    });
    connect(thumbBox, &QCheckBox::toggled, m_timeline, &TimelineWidget::setShowThumbnails);
    connect(previewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, previewCombo](int idx) {
        m_timeline->setPreviewSize(previewCombo->itemData(idx).toInt());
    });
    connect(zoomInBtn,  &QPushButton::clicked, m_timeline, &TimelineWidget::zoomIn);
    connect(zoomOutBtn, &QPushButton::clicked, m_timeline, &TimelineWidget::zoomOut);
    connect(fitBtn,     &QPushButton::clicked, m_timeline, &TimelineWidget::fitToContents);
    connect(m_timeline, &TimelineWidget::zoomChanged, this, [this](double msPerPixel) {
        m_zoomSlider->blockSignals(true);
        m_zoomSlider->setValue(zoomToSlider(msPerPixel));
        m_zoomSlider->blockSignals(false);
    });
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) {
        m_timeline->setMsPerPixel(sliderToZoom(v));
    });

    // ---------- 明亮轻快主题 ----------
    qApp->setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #f4f6fb; color: #3c4257;
            font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI"; font-size: 13px; }
        #topBar { background: #ffffff; border: 1px solid #e4e8f2; border-radius: 12px; }
        QPushButton { background: #ffffff; border: 1px solid #d9deeb; border-radius: 8px;
            padding: 6px 14px; color: #4a5170; }
        QPushButton:hover  { background: #eef1ff; border-color: #b9c2f2; color: #4c5bd5; }
        QPushButton:pressed { background: #e0e5ff; }
        QPushButton#primaryBtn { background: #6c6cf2; border: none; color: #ffffff; font-weight: bold; }
        QPushButton#primaryBtn:hover  { background: #7d7df6; }
        QPushButton#primaryBtn:pressed { background: #5a5ae0; }
        QLabel#pathLabel { color: #8a90a8; }
        QLabel#countLabel { background: #fff0e6; color: #e0722a; border-radius: 10px;
            padding: 3px 10px; font-weight: bold; }
        QCheckBox { spacing: 6px; color: #4a5170; }
        QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px;
            border: 1px solid #c3c9dd; background: #ffffff; }
        QCheckBox::indicator:checked { background: #6c6cf2; border-color: #6c6cf2;
            image: url(:/icons/check.png); }
        QCheckBox::indicator:hover { border-color: #6c6cf2; }
        QComboBox { background: #ffffff; border: 1px solid #d9deeb; border-radius: 8px;
            padding: 5px 10px; color: #4a5170; }
        QComboBox:hover { border-color: #b9c2f2; }
        QComboBox::drop-down { border: none; width: 22px; }
        QComboBox QAbstractItemView { background: #ffffff; border: 1px solid #d9deeb;
            selection-background-color: #eef1ff; selection-color: #4c5bd5; }
        QSlider::groove:horizontal { height: 6px; background: #e2e6f2; border-radius: 3px; }
        QSlider::sub-page:horizontal { background: #6c6cf2; border-radius: 3px; }
        QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0;
            border-radius: 8px; background: #ffffff; border: 2px solid #6c6cf2; }
        QSlider::handle:horizontal:hover { background: #eef1ff; }
        QStatusBar { background: #ffffff; color: #8a90a8; border-top: 1px solid #e4e8f2; }
        QToolTip { background: #ffffff; color: #3c4257; border: 1px solid #d9deeb;
            border-radius: 6px; padding: 6px; }
        QFileDialog QListView, QFileDialog QTreeView, QFileDialog QComboBox, QFileDialog QLineEdit {
            background: #ffffff; color: #3c4257; }
    )"));
}

MainWindow::~MainWindow() = default;

double MainWindow::sliderToZoom(int value) const
{
    const double t = double(value) / kSliderSteps;
    return std::pow(10.0, std::log10(kSliderMinZoom)
               + t * (std::log10(kSliderMaxZoom) - std::log10(kSliderMinZoom)));
}

int MainWindow::zoomToSlider(double zoom) const
{
    const double z = qBound(kSliderMinZoom, zoom, kSliderMaxZoom);
    const double t = (std::log10(z) - std::log10(kSliderMinZoom))
                   / (std::log10(kSliderMaxZoom) - std::log10(kSliderMinZoom));
    return int(std::lround(t * kSliderSteps));
}

void MainWindow::chooseDirectory()
{
    const QString start = m_currentDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        : m_currentDir;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要生成时间线的目录"), start);
    if (!dir.isEmpty())
        loadDirectory(dir);
}

void MainWindow::loadDirectory(const QString &path)
{
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    const QDirIterator::IteratorFlags flags = m_recursiveBox->isChecked()
        ? QDirIterator::Subdirectories
        : QDirIterator::NoIteratorFlags;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, flags);

    QVector<FileItem> files;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        FileItem item;
        item.name     = fi.fileName();
        item.path     = fi.absoluteFilePath();
        item.modified = fi.lastModified();
        item.size     = fi.size();
        item.suffix   = fi.suffix().toLower();
        files.push_back(item);
    }

    QGuiApplication::restoreOverrideCursor();

    m_currentDir = path;
    m_pathLabel->setText(path);
    m_pathLabel->setToolTip(path);
    m_timeline->setFiles(files); // 内部完成排序与自适应视图
    m_countLabel->setText(QStringLiteral("共 %1 个文件").arg(files.size()));

    const QVector<FileItem> &sorted = m_timeline->files();
    if (sorted.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("该目录下没有找到文件"));
    } else {
        statusBar()->showMessage(QStringLiteral("时间范围：%1  ~  %2")
            .arg(sorted.first().modified.toString("yyyy-MM-dd HH:mm:ss"),
                 sorted.last().modified.toString("yyyy-MM-dd HH:mm:ss")));
    }
}
