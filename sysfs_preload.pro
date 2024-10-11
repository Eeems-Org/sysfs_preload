TARGET = sysfs_preload
TEMPLATE = lib

QMAKE_RPATHDIR += /lib /usr/lib /opt/lib /opt/usr/lib
VERSION = 1.0.3

CONFIG += hide_symbols
CONFIG += c++17

QT =

SOURCES += main.cpp

LIBS += -lrt -ldl -Wl,--exclude-libs,ALL
LIBS += -lsystemd

target.path += /opt/lib
INSTALLS += target

xochitl_env.files = sysfs_preload.env
xochitl_env.path = /opt/etc/xochitl.env.d/
INSTALLS += xochitl_env

DEFINES += QT_MESSAGELOGCONTEXT
