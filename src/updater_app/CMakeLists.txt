set(VITA_UPDATER_TITLEID "VGUPDATE1")
add_executable(vitagram-updater main.cpp ${CMAKE_SOURCE_DIR}/src/package_promote.cpp ${CMAKE_SOURCE_DIR}/src/vita2d_texture_guard.cpp)
target_link_options(vitagram-updater PRIVATE "-Wl,--wrap=vita2d_create_empty_texture" "-Wl,--wrap=vita2d_create_empty_texture_format")
target_include_directories(vitagram-updater PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vitagram-updater
  vita2d
  ${WEBP_LIBRARY}
  freetype
  bz2
  png
  jpeg
  z
  m
  c
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  ScePromoterUtil_stub
)
vita_create_self(vitagram-updater-eboot.bin vitagram-updater UNSAFE)
vita_create_vpk(vitagram-updater.vpk ${VITA_UPDATER_TITLEID} vitagram-updater-eboot.bin
  VERSION ${VITA_VERSION}
  NAME "VitaGram Updater"
  FILE ${CMAKE_SOURCE_DIR}/assets/icon0.png sce_sys/icon0.png
)
