# Shared by the gate scripts: our C++ sources (tracked by git when there is a repository, else globbed).
function(cc_list_sources src out)
  set(files "")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${src}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} ls-files --cached --others --exclude-standard -- "*.cpp" "*.hpp" "*.h" "*.mm"
                    WORKING_DIRECTORY ${src} OUTPUT_VARIABLE listed OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
    if(rc EQUAL 0 AND listed)
      string(REPLACE "\n" ";" files "${listed}")
    endif()
  endif()
  if(NOT files)
    file(GLOB_RECURSE files RELATIVE ${src} ${src}/libs/*.cpp ${src}/libs/*.hpp ${src}/apps/*.cpp ${src}/apps/*.hpp
         ${src}/fuzz/*.cpp ${src}/tests/*.cpp)
  endif()
  list(FILTER files EXCLUDE REGEX "^(build|third_party|vendor)/")
  set(${out} "${files}" PARENT_SCOPE)
endfunction()
