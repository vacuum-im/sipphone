include(qmake/debug.inc)
include(qmake/config.inc)

#Project configuration
TARGET       = sipphone
QT           = core gui xml
INCLUDEPATH += .
include(sipphone.pri) 

#Project specific libs
win32 {
  DEFINES     += PJ_WIN32=1

  INCLUDEPATH += "$$PJ_DEV_DIR/msdirectx/include" \
                 "$$PJ_DEV_DIR/pjproject/include" \
                 "$$PJ_DEV_DIR/ffmpeg/include"

  LIBS        += -L"$$PJ_DEV_DIR/msdirectx/lib/x86" \
                 -L"$$PJ_DEV_DIR/pjproject/lib" \
                 -L"$$PJ_DEV_DIR/ffmpeg/lib"

  contains(DEFINES,DEBUG_MODE) {
    LIBS      += -llibpjproject-i386-Win32-vc8-Debug-Dynamic
  } else {
    LIBS      += -llibpjproject-i386-Win32-vc8-Release-Dynamic
  }

  LIBS        += -lWs2_32 -lole32 -loleaut32 -luuid -lodbc32 -lodbccp32 -lwinmm -lIphlpapi -ldsound -ldxguid -lnetapi32 -lmswsock -luser32 -lgdi32 -ladvapi32

  QMAKE_LFLAGS_RELEASE += /OPT:NOREF
}

#Default progect configuration
include(qmake/plugin.inc)

#Translation
TRANS_SOURCE_ROOT   = .
TRANS_BUILD_ROOT    = $${OUT_PWD}
include(translations/languages.inc)
