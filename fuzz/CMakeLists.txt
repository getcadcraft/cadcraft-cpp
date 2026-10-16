# One libFuzzer target per parser (AGENTS.md section 4). Corpus folders sit beside each target in fuzz/corpus/.
cc_fuzz(bit_reader SOURCES fuzz_bit_reader.cpp DEPS cc::base CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/bit_reader)
cc_fuzz(dwg SOURCES fuzz_dwg.cpp DEPS cc::io-dwg CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/dwg)
