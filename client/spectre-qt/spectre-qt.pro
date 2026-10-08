# Qt Creator convenience project. The real build is CMake-driven (see
# ../../CMakeLists.txt and README) -- this .pro exists only so spectre-qt can
# be opened/edited/debugged in Qt Creator without fighting its CMake
# project support. It links against libgdp's *already-built* static lib
# and generated protobuf headers rather than re-deriving libgdp's protoc
# build steps (libgdp/CMakeLists.txt) by hand, so:
#
#   run `make build` (or `cmake --build build`) from the repo root at least
#   once before opening this in Qt Creator, and again after touching
#   libgdp/ -- this project does not rebuild gdp itself.

QT += widgets
CONFIG += c++17

TARGET = spectre-qt
TEMPLATE = app

SOURCES += \
    src/main.cpp \
    src/connect_window.cpp \
    src/session_type_dialog.cpp \
    src/known_hosts.cpp \
    src/settings_dialog.cpp \
    ../spectre-settings/spectre_settings.cpp

HEADERS += \
    src/connect_window.h \
    src/session_type_dialog.h \
    src/fixed_size.h \
    src/known_hosts.h \
    src/settings_dialog.h \
    ../spectre-settings/spectre_settings.h

FORMS += \
    src/connect_window.ui \
    src/session_type_dialog.ui \
    src/settings_dialog.ui

RESOURCES += \
    resources/app.qrc

win32 {
    RC_FILE = resources/app.rc
}

INCLUDEPATH += \
    src \
    $$PWD/../../libgdp/include \
    $$PWD/../../build/libgdp/proto

LIBS += $$PWD/../../build/libgdp/libgdp.a

unix:!win32 {
    LIBS += -lprotobuf -lpthread -lzstd -lngtcp2 -lngtcp2_crypto_ossl -lssl -lcrypto
}
