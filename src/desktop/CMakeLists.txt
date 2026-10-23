cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(MarkSixStatisticsDesktop VERSION 0.1.0 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
elseif(NOT APPLE)
  message(FATAL_ERROR "Supported desktop targets are Windows and macOS")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.10)
enable_testing()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../statistics" "${CMAKE_BINARY_DIR}/statistics")

set(MARKSIX_MODEL_SOURCES
  models/statistics_models.cpp models/statistics_models.hpp
  models/statistics_loader.cpp models/statistics_loader_prepare.cpp models/statistics_loader.hpp
  models/statistics_data_root.cpp models/statistics_data_root.hpp)
set(MARKSIX_QML_FILES
  ui/Main.qml ui/StudioTheme.qml ui/ActionButton.qml ui/DataTable.qml
  ui/SelectableText.qml ui/ProgressChart.qml ui/DashboardPage.qml
  ui/ModelComparisonPage.qml ui/LearningPage.qml ui/DrawHistoryPage.qml
  ui/DateFilterPicker.qml
  ui/DataSourcesPage.qml)

qt_add_executable(MarkSixStudio WIN32 MACOSX_BUNDLE
  app/main.cpp app/desktop_app.cpp app/desktop_app.hpp
  app/desktop_options.cpp app/desktop_options.hpp
  app/desktop_diagnostics.cpp app/desktop_diagnostics.hpp
  ${MARKSIX_MODEL_SOURCES})
set_target_properties(MarkSixStudio PROPERTIES OUTPUT_NAME "mark-six-studio")
if(WIN32)
  set(MARKSIX_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/icon.ico")
  configure_file(app-icon.rc.in "${CMAKE_BINARY_DIR}/app-icon.rc" @ONLY)
  target_sources(MarkSixStudio PRIVATE "${CMAKE_BINARY_DIR}/app-icon.rc")
elseif(APPLE)
  set_target_properties(MarkSixStudio PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Mark Six Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.brucerry.marksixstudio"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../assets/icon.png" PROPERTIES QT_RESOURCE_ALIAS "icon.png")
qt_add_resources(MarkSixStudio "brand" PREFIX "/" FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/icon.png")
set_source_files_properties(ui/StudioTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(qml IN LISTS MARKSIX_QML_FILES)
  get_filename_component(qml_name "${qml}" NAME)
  set_source_files_properties("${qml}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
qt_add_qml_module(MarkSixStudio URI MarkSixStatisticsDesktop VERSION 1.0
  QML_FILES ${MARKSIX_QML_FILES})
target_include_directories(MarkSixStudio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/models")
target_link_libraries(MarkSixStudio PRIVATE marksix_statistics Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
if(MSVC)
  target_compile_options(MarkSixStudio PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(MarkSixStudio PRIVATE shell32)
else()
  target_compile_options(MarkSixStudio PRIVATE -Wall -Wextra)
endif()

qt_add_executable(MarkSixStatisticsQtModelTests
  tests/statistics_models_tests.cpp ${MARKSIX_MODEL_SOURCES})
target_include_directories(MarkSixStatisticsQtModelTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/models")
target_link_libraries(MarkSixStatisticsQtModelTests PRIVATE marksix_statistics Qt6::Core)
if(MSVC)
  target_compile_options(MarkSixStatisticsQtModelTests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(MarkSixStatisticsQtModelTests PRIVATE shell32)
else()
  target_compile_options(MarkSixStatisticsQtModelTests PRIVATE -Wall -Wextra)
endif()
add_test(NAME statistics_qt_model_adapters COMMAND MarkSixStatisticsQtModelTests)
set_tests_properties(statistics_qt_model_adapters PROPERTIES LABELS "statistics;qt" TIMEOUT 60)
