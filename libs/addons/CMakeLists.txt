# The add-on host (addons.js) and package trust (addons/trust.js). Signatures need libsodium; the web build has none and
# checks no signature (every installed package is refused as code there too).
set(addons_private "")
if(TARGET unofficial-sodium::sodium)
  set(addons_private unofficial-sodium::sodium)
endif()
cc_library(addons LAYER 2
  SOURCES src/loader.cpp src/trust.cpp
  DEPS cc::base
  PRIVATE_EXTERNAL ${addons_private})
target_compile_definitions(cc_addons PRIVATE CC_ADDONS_SODIUM=$<BOOL:${addons_private}>)

cc_test(addons
  SOURCES tests/trust_test.cpp tests/loader_test.cpp
  DEPS cc::addons)
if(TARGET test_addons)
  target_compile_definitions(test_addons PRIVATE
    CC_ADDON_GOLDEN="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-addon-trust.json"
    CC_ADDON_LOADER_GOLDEN="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-addon-loader.json")
endif()
