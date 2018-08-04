TARGET  = qheif

HEADERS += qheifhandler_p.h
SOURCES += main.cpp qheifhandler.cpp
OTHER_FILES += heif.json

include($$OUT_PWD/../../../imageformats/qtimageformats-config.pri)
QT_FOR_CONFIG += imageformats-private

QMAKE_USE_PRIVATE += heif

PLUGIN_TYPE = imageformats
PLUGIN_CLASS_NAME = QHeifPlugin
load(qt_plugin)
