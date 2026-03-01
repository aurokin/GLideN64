#-------------------------------------------------
#
# Project created by QtCreator 2015-01-26T21:59:49
#
#-------------------------------------------------

QT       += core gui
QTPLUGIN += qico

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = GLideNUI
TEMPLATE = lib
CONFIG += staticlib
CONFIG += c++11

SOURCES += \
	ConfigDialog.cpp \
	GLideNUI.cpp \
	FullscreenResolutions_windows.cpp \
	Settings.cpp \
	ScreenShot.cpp \
	AboutDialog.cpp \
	QtKeyToHID.cpp

HEADERS += \
	ConfigDialog.h \
	GLideNUI.h \
	FullscreenResolutions.h \
	Settings.h \
	AboutDialog.h

RESOURCES += \
	icon.qrc

FORMS += \
	configDialog.ui \
	AboutDialog.ui

TRANSLATIONS = realityvk_fr.ts \
               realityvk_de.ts \
               realityvk_it.ts \
               realityvk_es.ts \
               realityvk_pl.ts \
               realityvk_pt_BR.ts \
               realityvk_ja.ts

DISTFILES +=
