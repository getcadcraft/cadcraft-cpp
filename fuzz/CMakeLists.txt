# One libFuzzer target per parser (AGENTS.md section 4). Corpus folders sit beside each target in fuzz/corpus/.
cc_fuzz(bit_reader SOURCES fuzz_bit_reader.cpp DEPS cc::base CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/bit_reader)
cc_fuzz(dwg SOURCES fuzz_dwg.cpp DEPS cc::io-dwg CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/dwg)
cc_fuzz(dwg_write SOURCES fuzz_dwg_write.cpp DEPS cc::io-dwg CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/dwg)
cc_fuzz(json SOURCES fuzz_json.cpp DEPS cc::base CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/json)
cc_fuzz(dxf SOURCES fuzz_dxf.cpp DEPS cc::io-dxf CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/dxf)
cc_fuzz(compile SOURCES fuzz_compile.cpp DEPS cc::drawing cc::io-dxf CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/compile)
