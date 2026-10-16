# The browser build: cadcraft-web.{js,wasm,data} + index.html + cadcraft.css (forks/deploy.sh publishes that folder).
cc_executable(cadcraft-web
  SOURCES main.cpp
  DEPS cc::ui-imgui)
set_target_properties(cadcraft-web PROPERTIES SUFFIX ".js")
target_link_options(cadcraft-web PRIVATE
  "--preload-file=${PROJECT_SOURCE_DIR}/assets@/assets"
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=2MB
  -sPTHREAD_POOL_SIZE=4  # workers made at load, so the DWG reader can decode on threads without yielding (M3.5)
  "-sEXPORTED_FUNCTIONS=['_main','_cc_web_open','_malloc','_free']"
  "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','stringToNewUTF8','UTF8ToString']"
  -sENVIRONMENT=web,worker)
foreach(f index.html cadcraft.css)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${f} ${CMAKE_CURRENT_BINARY_DIR}/${f} COPYONLY)
endforeach()
