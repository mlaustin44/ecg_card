# ui_render_tool: renders the fw/ui screens for a set of scenarios to PNG.
#   cmake --build <dir> --target ui_render_images   -> build/render/*.png
get_filename_component(ECG_REPO ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_executable(ui_render_tool
  render.c
  png.c
  ${ECG_REPO}/fw/ui/tests/scenarios.c
)
target_include_directories(ui_render_tool PRIVATE ${ECG_REPO}/fw/ui/tests)
target_link_libraries(ui_render_tool PRIVATE ecg_ui m)

add_custom_target(ui_render_images
  COMMAND ui_render_tool ${ECG_REPO}/build/render
  WORKING_DIRECTORY ${ECG_REPO}
  DEPENDS ui_render_tool
  COMMENT "Rendering UI screens to build/render/")
