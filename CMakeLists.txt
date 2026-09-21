cmake_minimum_required(VERSION 3.19)

project(qt-kokoro-tts
    VERSION 0.1.0
    DESCRIPTION "Qt TextToSpeech plugin backed by Kokoro-82M"
    LANGUAGES CXX
)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Network Multimedia TextToSpeech)

qt_standard_project_setup()

qt_add_plugin(qtexttospeech_kokoro
    CLASS_NAME KokoroPlugin
    PLUGIN_TYPE texttospeech
)

target_sources(qtexttospeech_kokoro PRIVATE
    src/kokoroengine.cpp
    src/kokoroengine.h
    src/kokoroplugin.cpp
    src/kokoroplugin.h
)

target_include_directories(qtexttospeech_kokoro PRIVATE src)

# Register under the "speechd" key so applications that ask Qt for the default
# engine get Kokoro. Install such a build into a private directory and point
# QT_PLUGIN_PATH at it; it then shadows the real speechd plugin for that app
# only, leaving the system untouched.
option(KOKORO_TAKEOVER_SPEECHD "Claim the \"speechd\" engine key" OFF)

if(KOKORO_TAKEOVER_SPEECHD)
    target_compile_definitions(qtexttospeech_kokoro PRIVATE KOKORO_TAKEOVER_SPEECHD)
endif()

target_link_libraries(qtexttospeech_kokoro PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Multimedia
    Qt6::TextToSpeech
)

# Qt only discovers the plugin if it sits in <qt-plugins>/texttospeech.
if(NOT DEFINED QT_PLUGIN_INSTALL_DIR)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    execute_process(
        COMMAND "${_qmake}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE QT_PLUGIN_INSTALL_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

install(TARGETS qtexttospeech_kokoro
    LIBRARY DESTINATION "${QT_PLUGIN_INSTALL_DIR}/texttospeech"
)

option(BUILD_TOOLS "Build the kokoro-speak smoke-test tool" ON)

if(BUILD_TOOLS)
    qt_add_executable(kokoro-speak tools/kokoro-speak/main.cpp)
    target_link_libraries(kokoro-speak PRIVATE Qt6::Core Qt6::TextToSpeech)
    install(TARGETS kokoro-speak RUNTIME DESTINATION bin)
endif()
