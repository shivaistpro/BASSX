#[[
    BASSX — build

    Requisiti: CMake >= 3.22, compilatore C++17, JUCE 8 o superiore.

    Uso tipico (macOS):

        cmake -B build -DJUCE_PATH=/percorso/di/JUCE -DCMAKE_BUILD_TYPE=Release
        cmake --build build --config Release -j8

    Se JUCE non è presente:

        git clone --branch master --depth 1 https://github.com/juce-framework/JUCE.git

    Le texture in Assets/ sono generate da tools/generate_textures.py e vengono
    incorporate nel binario tramite juce_add_binary_data. Rigenerarle:

        python3 tools/generate_textures.py
]]

cmake_minimum_required(VERSION 3.22)

# Impostazioni macOS PRIMA di project(): dopo, CMake ha già fissato toolchain e
# architetture, e la prima build esce single-arch (era successo: binari solo arm64).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Versione minima di macOS")

# Universal binary su macOS: senza, il plugin non carica negli host Apple
# Silicon nativi se compilato su Intel e viceversa.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architetture macOS" FORCE)
endif()

# Windows: runtime C++ statico, così il plugin non richiede il VC++ Redistributable
# sulla macchina dell'utente.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Versione unica in VERSION: la leggono CMake, gli script di build e il plugin.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BASSX_VERSION LIMIT_COUNT 1)
string(STRIP "${BASSX_VERSION}" BASSX_VERSION)

project(BASSX VERSION ${BASSX_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: sorgenti in UTF-8 (commenti e stringhe con accenti) e compilazione parallela.
if(MSVC)
    add_compile_options(/utf-8 /MP)
endif()

# Copia automatica nelle cartelle di sistema solo su macOS: su Windows la cartella
# VST3 di sistema richiede privilegi di amministratore e farebbe fallire la build.
if(APPLE)
    set(BASSX_COPY_AFTER_BUILD TRUE)
else()
    set(BASSX_COPY_AFTER_BUILD FALSE)
endif()

# ------------------------------------------------------------------------------
# JUCE
# ------------------------------------------------------------------------------
# Percorso configurabile: di default si cerca una cartella JUCE accanto al
# progetto, poi una dentro di esso.
if(NOT DEFINED JUCE_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
        set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
        set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE/CMakeLists.txt")
        set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE")
    else()
        message(FATAL_ERROR
            "JUCE non trovato. Passa -DJUCE_PATH=/percorso/a/JUCE oppure clona JUCE "
            "dentro questa cartella.")
    endif()
endif()

message(STATUS "BASSX: uso JUCE da ${JUCE_PATH}")
add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")

# ------------------------------------------------------------------------------
# Plugin
# ------------------------------------------------------------------------------
juce_add_plugin(BASSX
    COMPANY_NAME                "BASSX Audio"
    BUNDLE_ID                   "com.bassxaudio.bassx"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     ${BASSX_COPY_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE    Bsxa
    PLUGIN_CODE                 Bsx1
    FORMATS                     VST3 AU Standalone
    VST3_CATEGORIES             "Instrument" "Synth"
    AU_MAIN_TYPE                "kAudioUnitType_MusicDevice"
    PRODUCT_NAME                "BASSX")

juce_generate_juce_header(BASSX)

# ------------------------------------------------------------------------------
# Texture incorporate
# ------------------------------------------------------------------------------
file(GLOB BASSX_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*.png")

if(NOT BASSX_ASSETS)
    message(FATAL_ERROR
        "Nessuna texture in Assets/. Eseguire prima: python3 tools/generate_textures.py")
endif()

juce_add_binary_data(BassXAssets SOURCES ${BASSX_ASSETS})

# Il target dei dati binari va compilato con -fPIC: verrà linkato dentro una
# libreria condivisa (il bundle VST3/AU).
set_target_properties(BassXAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# ------------------------------------------------------------------------------
# Sorgenti
# ------------------------------------------------------------------------------
target_sources(BASSX
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Parameters.cpp
        Source/DSP/WavetableBank.cpp
        Source/GUI/Pages.cpp
        Source/Presets/PresetManager.cpp

        # Unità header-only, elencate perché compaiano negli alberi di progetto
        # degli IDE.
        Source/PluginProcessor.h
        Source/PluginEditor.h
        Source/Parameters.h
        Source/DSP/Common.h
        Source/DSP/WavetableBank.h
        Source/DSP/Oscillator.h
        Source/DSP/Filters.h
        Source/DSP/Envelope.h
        Source/DSP/LFO.h
        Source/DSP/ModMatrix.h
        Source/DSP/Effects.h
        Source/DSP/Voice.h
        Source/DSP/Sequencer.h
        Source/DSP/NeuralDrive.h
        Source/DSP/NeuralDriveWeights.h
        Source/GUI/Assets.h
        Source/GUI/BassXLookAndFeel.h
        Source/GUI/Widgets.h
        Source/GUI/Pages.h
        Source/Presets/PresetManager.h)

target_include_directories(BASSX PRIVATE Source)

target_compile_definitions(BASSX
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_DSP_USE_SHARED_FFTW=0)

# Su Linux i backend audio nativi non servono al plugin (l'host fornisce
# l'audio) e le loro intestazioni di sviluppo non sono sempre installate.
if(UNIX AND NOT APPLE)
    target_compile_definitions(BASSX PUBLIC JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XSHM=0)
endif()

target_link_libraries(BASSX
    PRIVATE
        BassXAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ------------------------------------------------------------------------------
# Strumento di verifica dell'interfaccia (solo sviluppo)
# ------------------------------------------------------------------------------
option(BASSX_BUILD_GUI_RENDERER "Compila l'utility che salva uno screenshot della GUI" OFF)

if(BASSX_BUILD_GUI_RENDERER)
    juce_add_console_app(BassXRenderGui PRODUCT_NAME "BassXRenderGui")
    juce_generate_juce_header(BassXRenderGui)
    target_sources(BassXRenderGui PRIVATE
        tools/render/RenderGui.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Parameters.cpp
        Source/DSP/WavetableBank.cpp
        Source/GUI/Pages.cpp
        Source/Presets/PresetManager.cpp)
    target_include_directories(BassXRenderGui PRIVATE Source)
    target_compile_definitions(BassXRenderGui PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="BASSX" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
        JucePlugin_VersionString="${PROJECT_VERSION}" JucePlugin_Manufacturer="BASSX Audio")
    if(UNIX AND NOT APPLE)
        target_compile_definitions(BassXRenderGui PUBLIC JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XSHM=0)
    endif()
    target_link_libraries(BassXRenderGui PRIVATE BassXAssets juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)

    juce_add_console_app(BassXAudioTest PRODUCT_NAME "BassXAudioTest")
    juce_generate_juce_header(BassXAudioTest)
    target_sources(BassXAudioTest PRIVATE
        tools/render/AudioTest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Parameters.cpp
        Source/DSP/WavetableBank.cpp
        Source/GUI/Pages.cpp
        Source/Presets/PresetManager.cpp)
    target_include_directories(BassXAudioTest PRIVATE Source)
    target_compile_definitions(BassXAudioTest PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="BASSX" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
        JucePlugin_VersionString="${PROJECT_VERSION}" JucePlugin_Manufacturer="BASSX Audio")
    if(UNIX AND NOT APPLE)
        target_compile_definitions(BassXAudioTest PUBLIC JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XSHM=0)
    endif()
    target_link_libraries(BassXAudioTest PRIVATE BassXAssets juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()

# ------------------------------------------------------------------------------
# Calibrazione contro registrazioni hardware (solo sviluppo): BassXMatch
# ------------------------------------------------------------------------------
option(BASSX_BUILD_MATCH "Compila BassXMatch (render di pattern per la calibrazione)" OFF)
if(BASSX_BUILD_MATCH)
    juce_add_console_app(BassXMatch PRODUCT_NAME "BassXMatch")
    juce_generate_juce_header(BassXMatch)
    target_sources(BassXMatch PRIVATE
        tools/render/Match.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Parameters.cpp
        Source/DSP/WavetableBank.cpp
        Source/GUI/Pages.cpp
        Source/Presets/PresetManager.cpp)
    target_include_directories(BassXMatch PRIVATE Source)
    target_compile_definitions(BassXMatch PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="BASSX" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
        JucePlugin_VersionString="${PROJECT_VERSION}" JucePlugin_Manufacturer="BASSX Audio")
    if(UNIX AND NOT APPLE)
        target_compile_definitions(BassXMatch PUBLIC JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XSHM=0)
    endif()
    target_link_libraries(BassXMatch PRIVATE BassXAssets juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
