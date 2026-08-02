cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Hanbot 存档打包上传器 —— 客户端
#
# 设计目标：
#   1. 纯 Win32 API + C++17，零第三方依赖（压缩、加密、HTTP 全自研或用系统组件）
#   2. /MT 静态链接 CRT，目标机器不需要装任何 VC++ 运行库
#   3. 只用系统自带的 winhttp / comctl32 等 DLL
# ---------------------------------------------------------------------------

project(HanbotSavesUploader VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "本项目只支持 Windows 平台")
endif()

# ---- 静态链接 CRT：/MT（Release） /MTd（Debug）----
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- 默认 Release ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/config.cpp
    src/util.cpp
    src/json_mini.cpp
    src/deflate.cpp
    src/inflate.cpp
    src/zip_writer.cpp
    src/zip_reader.cpp
    src/http_client.cpp
    src/lol_finder.cpp
    src/uploader.cpp
)

set(APP_HEADERS
    src/resource.h
    src/config.h
    src/util.h
    src/json_mini.h
    src/deflate.h
    src/inflate.h
    src/zip_writer.h
    src/zip_reader.h
    src/http_client.h
    src/lol_finder.h
    src/uploader.h
)

set(APP_RESOURCES
    res/app.rc
)

# WIN32 => 生成 GUI 子系统程序，不弹黑框
add_executable(${PROJECT_NAME} WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${APP_RESOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

# ---------------------------------------------------------------------------
# 编译定义
# ---------------------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    STRICT
    WINVER=0x0A00          # Windows 10
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

# ---------------------------------------------------------------------------
# 系统库（全部是 Windows 自带的，不需要额外分发）
# ---------------------------------------------------------------------------
target_link_libraries(${PROJECT_NAME} PRIVATE
    winhttp       # HTTP 客户端
    comctl32      # 进度条 / 现代控件
    shlwapi       # 路径工具
    shell32       # 文件对话框、打开资源管理器
    ole32         # IFileDialog
    uuid
    advapi32      # 注册表、用户名
    user32
    gdi32
    bcrypt        # 加密安全随机数
    version
    uxtheme
    ws2_32        # GetMachineIp 用的 Winsock（ntohl 等）
    iphlpapi      # GetAdaptersAddresses 取本机 IP
)

# ---------------------------------------------------------------------------
# MSVC 专属设置
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /utf-8                # 源码与执行字符集都按 UTF-8 处理，中文不乱码
        /W3
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP                   # 多核编译
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/GL>   # 全程序优化
        $<$<CONFIG:Release>:/Gy>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        /MANIFEST:NO          # 清单由 res/app.rc 嵌入，避免两份清单冲突
        /SUBSYSTEM:WINDOWS
        /ENTRY:wWinMainCRTStartup
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
    )

    # rc.exe 默认按系统 ANSI 代码页解析，显式指定 UTF-8，
    # 否则 GitHub 英文环境的构建机上版本信息里的中文会变乱码
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /nologo /c65001")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "HanbotSavesUploader"
    VS_DPI_AWARE "PerMonitor"
)

# ---------------------------------------------------------------------------
# 安装 / 打包产物
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)

message(STATUS "----------------------------------------------------")
message(STATUS " ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS " 运行库     : ${CMAKE_MSVC_RUNTIME_LIBRARY} (静态)")
message(STATUS " 生成器     : ${CMAKE_GENERATOR}")
message(STATUS "----------------------------------------------------")
