#@author: Deng Zhimao
#@email: dengzhimao@alientek.com
#B站作品: https://space.bilibili.com/474103963?spm_id_from=333.788.0.0

#开源电子网: http://www.openedv.com/forum.php
#正点原子官网: https://www.alientek.com
#店铺: https://zhengdianyuanzi.tmall.com

#LICENCE GPLV3 
cmake_minimum_required(VERSION 3.8)

project(rv1126b_desktop)
set(DESKTOP_TARGET rv1126b_desktop)
set(TOOLCHAIN_DIR /opt/atk-dlrv1126b-toolchain)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-g++)
set(CMAKE_C_COMPILER ${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-gcc)
set(SYSROOT ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot/usr/include)
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot)


include_directories(
        ${CMAKE_SYSROOT}/usr/include/lvgl
        ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
        ${CMAKE_SYSROOT}/usr/include/c++/13.3.0
        )

add_definitions(-DUSE_DRM=1 -DUSE_EVDEV=1)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/lvgl8)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/apps)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/common)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/ocr)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/detect)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/camera)
include_directories(${PROJECT_SOURCE_DIR}/lvgl_desktop/icon)
include_directories(${PROJECT_SOURCE_DIR}/common)
include_directories(${PROJECT_SOURCE_DIR}/sys)
include_directories(${PROJECT_SOURCE_DIR}/services)
include_directories(${PROJECT_SOURCE_DIR}/services/camera)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/ocr)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/ocr/3rdparty/rknpu2/include)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/detect)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/hair)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/face)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/plate)
include_directories(${PROJECT_SOURCE_DIR}/services/algorithms/seg)

# OpenCV 头位于 sysroot/usr/include/opencv4
include_directories(${CMAKE_SYSROOT}/usr/include/opencv4)

aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl8 LVGL_PORT_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/sys SYS_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common COMMON_DIR_SRCS)
set(COMMON_SRCS
        ${LVGL_PORT_SRCS}
        ${SYS_SRCS}
        ${COMMON_DIR_SRCS}
)

aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop DESKTOP_CORE_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/apps DESKTOP_APP_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/common DESKTOP_COMPONENT_COMMON_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/ocr DESKTOP_COMPONENT_OCR_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/detect DESKTOP_COMPONENT_DETECT_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/components/camera DESKTOP_COMPONENT_CAMERA_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/lvgl_desktop/icon DESKTOP_ICON_SRCS)
set(DESKTOP_COMPONENT_SRCS
        ${DESKTOP_COMPONENT_COMMON_SRCS}
        ${DESKTOP_COMPONENT_OCR_SRCS}
        ${DESKTOP_COMPONENT_DETECT_SRCS}
        ${DESKTOP_COMPONENT_CAMERA_SRCS}
        ${DESKTOP_ICON_SRCS}
)
set(DESKTOP_SRCS
        ${DESKTOP_CORE_SRCS}
        ${DESKTOP_APP_SRCS}
        ${DESKTOP_COMPONENT_SRCS}
)

aux_source_directory(${PROJECT_SOURCE_DIR}/services/camera SERVICE_CAMERA_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms SERVICE_ALGO_CORE_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/ocr SERVICE_OCR_ALGO_SRCS)
# detect 目录下既有 detect_algo.cc / yolov8.cc / postprocess.cc 这些 RKNN 推理文件
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/detect SERVICE_DETECT_ALGO_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/hair SERVICE_HAIR_ALGO_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/face SERVICE_FACE_ALGO_SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/plate SERVICE_PLATE_ALGO_SRCS)
list(REMOVE_ITEM SERVICE_PLATE_ALGO_SRCS
        ${PROJECT_SOURCE_DIR}/services/algorithms/plate/plate_test.cc)
aux_source_directory(${PROJECT_SOURCE_DIR}/services/algorithms/seg SERVICE_SEG_ALGO_SRCS)
set(SERVICE_SRCS
        ${SERVICE_CAMERA_SRCS}
        ${SERVICE_ALGO_CORE_SRCS}
        ${SERVICE_OCR_ALGO_SRCS}
        ${SERVICE_DETECT_ALGO_SRCS}
        ${SERVICE_HAIR_ALGO_SRCS}
        ${SERVICE_FACE_ALGO_SRCS}
        ${SERVICE_PLATE_ALGO_SRCS}
        ${SERVICE_SEG_ALGO_SRCS}
)

add_executable(${DESKTOP_TARGET}
        ${COMMON_SRCS}
        ${DESKTOP_SRCS}
        ${SERVICE_SRCS}
        ${PROJECT_SOURCE_DIR}/desktop.c
)

target_link_libraries(${DESKTOP_TARGET}
        lvgl lv_drivers freetype drm evdev pthread jpeg
        rknnrt
        opencv_core opencv_imgproc opencv_imgcodecs
)

# 板端离线测试：不链接 LVGL，仅 plate RKNN 推理
set(PLATE_TEST_SRCS
        ${PROJECT_SOURCE_DIR}/services/algorithms/plate/yolov8_plate.cc
        ${PROJECT_SOURCE_DIR}/services/algorithms/plate/postprocess.cc
        ${PROJECT_SOURCE_DIR}/services/algorithms/plate/plate_test.cc
)
add_executable(plate_test ${PLATE_TEST_SRCS})
target_link_libraries(plate_test
        rknnrt
        opencv_core opencv_imgproc opencv_imgcodecs
        pthread
)

add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E remove CMakeCache.txt
    COMMAND ${CMAKE_COMMAND} -E remove Makefile
    COMMAND ${CMAKE_COMMAND} -E remove_directory CMakeFiles
    COMMAND ${CMAKE_COMMAND} -E remove cmake_install.cmake
    COMMAND ${CMAKE_COMMAND} -E remove ${DESKTOP_TARGET}
    COMMAND ${CMAKE_COMMAND} -E remove 02_lvgl_button_led
    COMMENT "Clean all CMake generated files and desktop target"
)
