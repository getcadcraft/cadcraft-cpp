# Code the W5 apps (CivilCraft, DetailCraft, MarkupCraft, TakeoffCraft) share: the 0.5 files that are byte-identical in
# several app folders (lasso-core.js) and JS semantics their ports need.
cc_library(appkit LAYER 2
  SOURCES src/lasso.cpp
  DEPS cc::base)

cc_test(appkit
  SOURCES tests/appkit_test.cpp
  DEPS cc::appkit)
