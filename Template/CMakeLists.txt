cmake_minimum_required(VERSION 3.16)
project(Time_tool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 不写死路径，优先使用命令行传入的 CMAKE_PREFIX_PATH 或环境变量 QT_PATH
if(NOT CMAKE_PREFIX_PATH)
    if(DEFINED ENV{QT_PATH})
        set(CMAKE_PREFIX_PATH "$ENV{QT_PATH}")
    endif()
endif()

# 如果仍然没有找到，可以尝试从系统变量寻找，或者保持为空让 find_package 报错

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR})

if(MSVC)
    add_compile_options(
        $<$<CONFIG:Release>:/MT>
        $<$<CONFIG:Debug>:/MTd>
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
    )
    add_link_options(
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core Svg)

# 包含 main.cpp、Windows 图标资源 (.rc) 以及 Qt 内嵌资源 (.qrc)
add_executable(Time_tool WIN32 
    src/main.cpp
    resources/app.rc
    resources/resources.qrc
)

# [Build Optimization] Try to close running instances before building to solve LNK1104 file locking issues
if(WIN32)
    add_custom_command(TARGET Time_tool PRE_BUILD
        COMMAND taskkill /F /IM Time_tool.exe /T >nul 2>nul || (exit 0)
        COMMENT "Attempting to close running Time_tool instances to release file lock..."
    )
endif()

target_link_libraries(Time_tool PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Svg
)

if(WIN32)
    target_link_libraries(Time_tool PRIVATE
        User32 Gdi32 Shell32 Ole32
        Comdlg32 Winspool Winmm Imm32
        Dwmapi Uxtheme Ws2_32
        Setupapi Netapi32 Userenv
        Version Crypt32
    )
endif()
