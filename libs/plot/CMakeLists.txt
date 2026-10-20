cc_library(plot LAYER 5
  SOURCES src/plot.cpp
  DEPS cc::base cc::cad cc::doc cc::docview cc::sheets)

cc_test(plot
  SOURCES tests/plot_test.cpp
  DEPS cc::plot cc::sheets cc::doc cc::i18n)
