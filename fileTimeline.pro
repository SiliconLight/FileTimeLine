QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/rawpreview.cpp \
    src/timelinewidget.cpp

HEADERS += \
    src/mainwindow.h \
    src/rawpreview.h \
    src/timelinewidget.h

RESOURCES += \
    resources/resources.qrc

RC_FILE = resources/fileTimeline.rc

# 翻译：lrelease 自动编译 .ts 并嵌入到 :/i18n/
TRANSLATIONS += translations/fileTimeline_zh_CN.ts
CONFIG += lrelease
CONFIG += embed_translations

# 静态 Qt 构建时连 MinGW 运行库也静态化，产出免安装单 exe
static {
    QMAKE_LFLAGS += -static -static-libgcc -static-libstdc++
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
