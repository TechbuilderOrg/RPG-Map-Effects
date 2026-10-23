# In-tree build: when built inside obs-studio/plugins/, use this minimal setup.
# Standalone builds use the OBS plugin template (see else branch below).
cmake_minimum_required(VERSION 3.22...3.30)

# Detect in-tree build: OBS::libobs exists when we're part of obs-studio
if(TARGET OBS::libobs)
  # --- In-tree build (no project() - we're a subdirectory of obs-studio) ---
  find_package(Python3 REQUIRED)

  if(COMMAND legacy_check)
    legacy_check()
  endif()

  add_library(rpg-map-effects MODULE)
  add_library(OBS::rpg-map-effects ALIAS rpg-map-effects)

  # Embed cursor PNGs into C++ at build time (generated source)
  set(CURSOR_ASSETS_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/cursor_assets_generated.cpp")
  file(GLOB CURSOR_PNGS "${CMAKE_CURRENT_SOURCE_DIR}/cursors/*.png")
  add_custom_command(
    OUTPUT "${CURSOR_ASSETS_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -E env PYTHONIOENCODING=utf-8
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_cursors.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/cursors"
      "${CURSOR_ASSETS_GENERATED}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_cursors.py" ${CURSOR_PNGS}
    COMMENT "Embedding cursor PNGs into C++"
  )

  target_sources(rpg-map-effects PRIVATE
    src/plugin-main.cpp
    src/rpg_window.cpp
    src/rpg_fog_source.cpp
    src/obs_display_widget.mm
    "${CURSOR_ASSETS_GENERATED}"
  )

  target_include_directories(rpg-map-effects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(rpg-map-effects PRIVATE cxx_std_17)

  target_link_libraries(rpg-map-effects PRIVATE
    OBS::libobs
    OBS::frontend-api
    Qt6::Core
    Qt6::Widgets
  )

  target_compile_options(rpg-map-effects PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )

  if(COMMAND set_target_properties_obs)
    set_target_properties_obs(rpg-map-effects PROPERTIES
      FOLDER plugins
      PREFIX ""
    )
  else()
    set_target_properties(rpg-map-effects PROPERTIES PREFIX "")
  endif()
  set_target_properties(rpg-map-effects PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

  # Allow plugin to load when placed inside OBS.app (PlugIns folder)
  if(APPLE)
    set_target_properties(rpg-map-effects PROPERTIES
      XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS
      "@executable_path/../Frameworks @executable_path/../../../../Frameworks"
    )
  endif()
else()
  # --- Standalone build (OBS plugin template) ---
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

  project(${_name} VERSION ${_version} LANGUAGES C CXX OBJC OBJCXX)

  option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
  option(ENABLE_QT "Use Qt functionality" ON)

  include(compilerconfig)
  include(defaults)
  include(helpers)

  add_library(${CMAKE_PROJECT_NAME} MODULE)

  find_package(libobs REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

  if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  endif()

  if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(
      ${CMAKE_PROJECT_NAME}
      PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
      ${CMAKE_PROJECT_NAME}
      PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
  endif()

  find_package(Python3 REQUIRED)
  set(CURSOR_ASSETS_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/cursor_assets_generated.cpp")
  file(GLOB CURSOR_PNGS "${CMAKE_CURRENT_SOURCE_DIR}/cursors/*.png")
  add_custom_command(
    OUTPUT "${CURSOR_ASSETS_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -E env PYTHONIOENCODING=utf-8
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_cursors.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/cursors"
      "${CURSOR_ASSETS_GENERATED}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_cursors.py" ${CURSOR_PNGS}
    COMMENT "Embedding cursor PNGs into C++"
  )

  target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/rpg_window.cpp
    src/rpg_fog_source.cpp
    src/obs_display_widget.mm
    "${CURSOR_ASSETS_GENERATED}"
  )

  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

  if(APPLE)
    set_target_properties(
      ${CMAKE_PROJECT_NAME}
      PROPERTIES
        XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS
        "@executable_path/../Frameworks @executable_path/../../../../Frameworks"
    )
  endif()
endif()
