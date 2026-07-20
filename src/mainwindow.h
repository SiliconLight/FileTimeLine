#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTranslator>

class TimelineWidget;
class QLabel;
class QSlider;
class QCheckBox;
class QComboBox;
class QPushButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void chooseDirectory();
    void loadDirectory(const QString &path);

private:
    double sliderToZoom(int value) const;
    int    zoomToSlider(double zoom) const;

    void   retranslateUi();                       // 语言切换后重设所有界面文字
    void   refreshStatusBar();                    // 按当前状态刷新状态栏文字
    void   applyLanguage(const QString &lang);    // 安装/移除翻译器并刷新界面
    static QString initialLanguage();             // 读设置，缺省跟随系统语言

    TimelineWidget *m_timeline;
    QLabel    *m_pathLabel;
    QLabel    *m_countLabel;
    QCheckBox *m_recursiveBox;
    QCheckBox *m_thumbBox;
    QComboBox *m_previewCombo;
    QComboBox *m_langCombo;
    QPushButton *m_openBtn;
    QPushButton *m_zoomOutBtn;
    QPushButton *m_zoomInBtn;
    QPushButton *m_fitBtn;
    QSlider   *m_zoomSlider;
    QString    m_currentDir;
    QString    m_lang;
    QTranslator m_translator;                     // 生命周期须覆盖翻译器使用期
};

#endif // MAINWINDOW_H
