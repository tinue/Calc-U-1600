# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles/CalcU1600Qt_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/CalcU1600Qt_autogen.dir/ParseCache.txt"
  "CalcU1600Qt_autogen"
  )
endif()
