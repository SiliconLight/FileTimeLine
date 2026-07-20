#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setStyle("Fusion"); // 保证亮色 qss 在各平台渲染一致
    QApplication::setOrganizationName(QStringLiteral("SiliconLight")); // QSettings 用
    QApplication::setApplicationName(QStringLiteral("fileTimeline"));
    a.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    MainWindow w;
    w.show();
    return a.exec();
}
