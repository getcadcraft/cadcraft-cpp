# The secure channel (crypto.cpp, LIVE-CRYPTO.md) and the session server (server.cpp, session.rs) are on libsodium; the
# web build leaves them out (the browser edition is 0.5's JS).
set(live_sources src/msgpack.cpp src/envelope.cpp src/session_core.cpp)
set(live_private "")
if(TARGET unofficial-sodium::sodium)
  list(APPEND live_sources src/crypto.cpp src/server.cpp)
  set(live_private unofficial-sodium::sodium)
endif()
cc_library(live LAYER 2
  SOURCES ${live_sources}
  DEPS cc::base
  PRIVATE_EXTERNAL ${live_private})

cc_test(live
  SOURCES tests/vectors_test.cpp
  DEPS cc::live)
if(TARGET test_live)
  target_compile_definitions(test_live PRIVATE CC_LIVE_VECTORS="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-live-vectors.json")
endif()
if(TARGET unofficial-sodium::sodium)
  cc_test(live-crypto
    SOURCES tests/crypto_test.cpp tests/server_test.cpp
    DEPS cc::live)
  if(TARGET test_live-crypto)
    target_link_libraries(test_live-crypto PRIVATE unofficial-sodium::sodium)
    target_compile_definitions(test_live-crypto PRIVATE
      CC_LIVE_CRYPTO_VECTORS="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-live-crypto-vectors.json"
      CC_LIVE_VECTORS="${PROJECT_SOURCE_DIR}/tests/golden/js-0.5-live-vectors.json")
  endif()
endif()
