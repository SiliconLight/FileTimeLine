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
#include <QLocale>
#include <QPushButton>
#include <QSettings>
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
    resize(1200, 680);

    // ---------- 顶部工具条 ----------
    auto *bar = new QWidget;
    bar->setObjectName("topBar");
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(14, 10, 14, 10);
    barLayout->setSpacing(10);

    m_openBtn = new QPushButton;
    m_openBtn->setObjectName("primaryBtn");

    m_pathLabel = new QLabel;
    m_pathLabel->setObjectName("pathLabel");
    m_pathLabel->setMinimumWidth(160);
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_recursiveBox = new QCheckBox;
    m_recursiveBox->setChecked(false);

    m_thumbBox = new QCheckBox;
    m_thumbBox->setChecked(true);

    m_previewCombo = new QComboBox;
    m_previewCombo->addItem(QString(), 240);
    m_previewCombo->addItem(QString(), 400);
    m_previewCombo->addItem(QString(), 560);
    m_previewCombo->setCurrentIndex(1); // 默认中号

    m_zoomOutBtn = new QPushButton(QStringLiteral("−"));
    m_zoomInBtn  = new QPushButton(QStringLiteral("＋"));
    m_zoomOutBtn->setFixedWidth(36);
    m_zoomInBtn->setFixedWidth(36);

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(0, kSliderSteps);
    m_zoomSlider->setFixedWidth(150);
    m_zoomSlider->setFixedHeight(22); // 给手柄圆点留足高度，避免上下被裁剪

    m_fitBtn = new QPushButton;

    m_countLabel = new QLabel;
    m_countLabel->setObjectName("countLabel");

    m_langCombo = new QComboBox;
    m_langCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh_CN"));
    m_langCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));

    barLayout->addWidget(m_openBtn);
    barLayout->addWidget(m_pathLabel, 1);
    barLayout->addWidget(m_recursiveBox);
    barLayout->addWidget(m_thumbBox);
    barLayout->addWidget(m_previewCombo);
    barLayout->addSpacing(8);
    barLayout->addWidget(m_zoomOutBtn);
    barLayout->addWidget(m_zoomSlider);
    barLayout->addWidget(m_zoomInBtn);
    barLayout->addWidget(m_fitBtn);
    barLayout->addSpacing(8);
    barLayout->addWidget(m_countLabel);
    barLayout->addWidget(m_langCombo);

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

    // ---------- 信号连接 ----------
    connect(m_openBtn, &QPushButton::clicked, this, &MainWindow::chooseDirectory);
    connect(m_recursiveBox, &QCheckBox::toggled, this, [this] {
        if (!m_currentDir.isEmpty())
            loadDirectory(m_currentDir);
    });
    connect(m_thumbBox, &QCheckBox::toggled, m_timeline, &TimelineWidget::setShowThumbnails);
    connect(m_previewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
        m_timeline->setPreviewSize(m_previewCombo->itemData(idx).toInt());
    });
    connect(m_langCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { applyLanguage(m_langCombo->itemData(idx).toString()); });
    connect(m_zoomInBtn,  &QPushButton::clicked, m_timeline, &TimelineWidget::zoomIn);
    connect(m_zoomOutBtn, &QPushButton::clicked, m_timeline, &TimelineWidget::zoomOut);
    connect(m_fitBtn,     &QPushButton::clicked, m_timeline, &TimelineWidget::fitToContents);
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

    // 所有界面文字统一由 retranslateUi 设置（含初始语言）
    applyLanguage(initialLanguage());
}

MainWindow::~MainWindow() = default;

QString MainWindow::initialLanguage()
{
    const QString saved = QSettings().value(QStringLiteral("language")).toString();
    if (saved == QLatin1String("zh_CN") || saved == QLatin1String("en"))
        return saved;
    // 缺省跟随系统语言：中文系统用中文，其余用英文
    return QLocale::system().name().startsWith(QLatin1String("zh"))
        ? QStringLiteral("zh_CN") : QStringLiteral("en");
}

void MainWindow::applyLanguage(const QString &lang)
{
    qApp->removeTranslator(&m_translator);
    if (lang == QLatin1String("zh_CN")
        && m_translator.load(QStringLiteral(":/i18n/fileTimeline_zh_CN.qm")))
        qApp->installTranslator(&m_translator);
    m_lang = lang;
    QSettings().setValue(QStringLiteral("language"), lang);
    retranslateUi();
}

void MainWindow::retranslateUi()
{
    setWindowTitle(tr("File Timeline"));
    m_openBtn->setText(tr("Open Folder"));
    if (m_currentDir.isEmpty())
        m_pathLabel->setText(tr("No folder selected"));
    m_recursiveBox->setText(tr("Include subfolders"));
    m_thumbBox->setText(tr("Thumbnails"));
    m_thumbBox->setToolTip(tr("Show image thumbnails on the timeline; hover for a large preview"));
    m_previewCombo->setItemText(0, tr("Small preview"));
    m_previewCombo->setItemText(1, tr("Medium preview"));
    m_previewCombo->setItemText(2, tr("Large preview"));
    m_previewCombo->setToolTip(tr("Hover preview size"));
    m_zoomOutBtn->setToolTip(tr("Zoom out"));
    m_zoomInBtn->setToolTip(tr("Zoom in"));
    m_zoomSlider->setToolTip(tr("Zoom"));
    m_fitBtn->setText(tr("Fit All"));
    m_fitBtn->setToolTip(tr("Zoom and pan to fit all files"));
    m_langCombo->setToolTip(tr("Interface language"));
    m_langCombo->blockSignals(true);
    m_langCombo->setCurrentIndex(m_lang == QLatin1String("zh_CN") ? 0 : 1);
    m_langCombo->blockSignals(false);
    if (!m_currentDir.isEmpty())
        m_countLabel->setText(tr("%1 files").arg(m_timeline->files().size()));
    refreshStatusBar();
    m_timeline->update(); // 时间线内文字在绘制时取 tr()，重绘即可
}

void MainWindow::refreshStatusBar()
{
    if (m_currentDir.isEmpty()) {
        statusBar()->showMessage(
            tr("Ready - wheel to zoom, drag to pan, hover for file details"));
    } else if (m_timeline->files().isEmpty()) {
        statusBar()->showMessage(tr("No files found in this folder"));
    } else {
        const QVector<FileItem> &sorted = m_timeline->files();
        statusBar()->showMessage(tr("Time range: %1  ~  %2")
            .arg(sorted.first().modified.toString("yyyy-MM-dd HH:mm:ss"),
                 sorted.last().modified.toString("yyyy-MM-dd HH:mm:ss")));
    }
}

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
        this, tr("Choose a folder to visualize"), start);
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
    m_countLabel->setText(tr("%1 files").arg(files.size()));
    refreshStatusBar();
}
