# FindRubberBand.cmake
#
# Placeholder for step 2. Rubber Band ships a pkg-config file
# (rubberband.pc) on Ubuntu 24.04, so the simplest path is:
#
#   find_package(PkgConfig REQUIRED)
#   pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
#
# This stub exists so that adding a `find_package(RubberBand)` call
# in the top-level CMakeLists.txt during step 2 doesn't break anyone
# building from a fresh checkout — replace its body with something
# real (or delete it and use pkg-config directly) when wiring step 2.

message(WARNING "FindRubberBand.cmake is a step-2 placeholder; "
                "use pkg_check_modules(rubberband) when ready.")
