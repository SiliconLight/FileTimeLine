#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class TimelineWidget;
class QLabel;
class QSlider;
class QCheckBox;

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

    TimelineWidget *m_timeline;
    QLabel   *m_pathLabel;
    QLabel   *m_countLabel;
    QCheckBox *m_recursiveBox;
    QSlider  *m_zoomSlider;
    QString   m_currentDir;
};

#endif // MAINWINDOW_H
