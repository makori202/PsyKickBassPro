cmake_minimum_required(VERSION 3.22)
project(PsyKickBassPro VERSION 6.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
)
FetchContent_MakeAvailable(JUCE)

link_directories("${CMAKE_BINARY_DIR}/../packages/Microsoft.Web.WebView2/build/native/x64")

juce_add_plugin(PsyKickBassPro
    COMPANY_NAME "GoaArchitect"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE GoaA
    PLUGIN_CODE PKBP
    FORMATS VST3 Standalone
    PRODUCT_NAME "PsyKickBassPro"
)

juce_add_binary_data(PsyKickBassProData SOURCES
    Resources/ui.html
)

target_sources(PsyKickBassPro PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(PsyKickBassPro PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PsyKickBassPro PRIVATE
    PsyKickBassProData
    WebView2LoaderStatic.lib
    juce::juce_audio_utils
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
