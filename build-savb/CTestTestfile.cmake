# CMake generated Testfile for 
# Source directory: /home/sav-sys-1027/PlotJuggler/plugins/data_load_savb
# Build directory: /home/sav-sys-1027/PlotJuggler/build-savb
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[savb_source_test]=] "/home/sav-sys-1027/PlotJuggler/build-savb/savb_source_test")
set_tests_properties([=[savb_source_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/sav-sys-1027/PlotJuggler/plugins/data_load_savb/CMakeLists.txt;96;add_test;/home/sav-sys-1027/PlotJuggler/plugins/data_load_savb/CMakeLists.txt;0;")
subdirs("_deps/nlohmann_json-build")
subdirs("_deps/fastfloat-build")
subdirs("_deps/plotjuggler_sdk-build")
