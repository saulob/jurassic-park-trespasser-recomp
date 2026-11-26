# Squirrel (config/launcher_deps.txt, patched by launcher/patches/squirrel) and the interpreter of Trespasser CE scripts
# over it (docs/CE_SCRIPTS.md): the language and its math, string and blob libraries, no I/O, system or auxiliary library,
# allocations charged to the VM's heap (src/common/jpt_script_heap.cpp). Nothing of the game links it yet.
set(JPT_SQUIRREL_DIR "${CMAKE_SOURCE_DIR}/external/squirrel" CACHE PATH "patched Squirrel checkout (config/launcher_deps.txt)")
if(NOT EXISTS "${JPT_SQUIRREL_DIR}/include/squirrel.h")
  message(FATAL_ERROR "Squirrel sources missing: run tools/build.ps1 or tools/fetch_launcher_deps.py (config/launcher_deps.txt)")
endif()

# A refused allocation is a C++ exception that crosses Squirrel's extern "C" API; /EHsc would end the process there.
if(MSVC)
  string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

set(squirrel_sources)
foreach(name sqapi sqbaselib sqclass sqcompiler sqdebug sqfuncstate sqlexer sqmem sqobject sqstate sqtable sqvm)
  list(APPEND squirrel_sources ${JPT_SQUIRREL_DIR}/squirrel/${name}.cpp)
endforeach()
foreach(name sqstdblob sqstdmath sqstdrex sqstdstream sqstdstring)
  list(APPEND squirrel_sources ${JPT_SQUIRREL_DIR}/sqstdlib/${name}.cpp)
endforeach()
set(script_sources ${CMAKE_SOURCE_DIR}/src/common/jpt_script_heap.cpp ${CMAKE_SOURCE_DIR}/src/common/jpt_script_vm.cpp)

add_library(jpt_script STATIC EXCLUDE_FROM_ALL ${squirrel_sources} ${script_sources})
target_include_directories(jpt_script SYSTEM PUBLIC ${JPT_SQUIRREL_DIR}/include)
target_include_directories(jpt_script PUBLIC ${CMAKE_SOURCE_DIR}/src/common)
target_compile_definitions(jpt_script PRIVATE SQ_EXCLUDE_DEFAULT_MEMFUNCTIONS)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(jpt_script PUBLIC _SQ64)
endif()
set_target_properties(jpt_script PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set_source_files_properties(${squirrel_sources} PROPERTIES COMPILE_OPTIONS "/W0")
  set_source_files_properties(${script_sources} PROPERTIES COMPILE_OPTIONS "/W4;/WX;/utf-8")
  target_compile_definitions(jpt_script PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  set_source_files_properties(${squirrel_sources} PROPERTIES COMPILE_OPTIONS "-w")
  set_source_files_properties(${script_sources} PROPERTIES COMPILE_OPTIONS "${JPT_LINUX_WARNINGS};${JPT_LINUX_COMPAT}")
endif()
