include ( ../../settings.pro )
include ( ../../version.pro )
include ( ../programs-libs.pro )

QT += network xml sql

TEMPLATE = app
CONFIG += thread
CONFIG -= moc
TARGET = mythfilldatabase
target.path = $${PREFIX}/bin
INSTALLS = target

QMAKE_CLEAN += $(TARGET)

INCLUDEPATH += ../../libs/libmythtv/mpeg /usr/include/qjson
DEPENDPATH  += ../../libs/libmythtv/mpeg
LIBS += -lqjson


# Input
HEADERS += filldata.h   channeldata.h
HEADERS += icondata.h   xmltvparser.h
HEADERS += fillutil.h   commandlineparser.h
SOURCES += filldata.cpp channeldata.cpp
SOURCES += icondata.cpp xmltvparser.cpp
SOURCES += fillutil.cpp
SOURCES += main.cpp     commandlineparser.cpp
