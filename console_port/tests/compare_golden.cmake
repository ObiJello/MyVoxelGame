# Cross-compiler determinism: the port's generation output must hash to the
# value recorded from the reference platform. A mismatch usually means code
# that depends on unspecified evaluation order (e.g. two Random calls in one
# argument list), which makes the same seed build a different world.
execute_process(COMMAND "${PORTED}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Generation sample failed (${status}): ${error}")
endif()
string(SHA256 actual "${output}")
if(NOT actual STREQUAL EXPECTED)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/golden-actual.txt" "${output}")
  message(FATAL_ERROR "Generation output differs from the recorded world: ${PORTED}\n  expected ${EXPECTED}\n  actual   ${actual}")
endif()
message(STATUS "Generation output matches the recorded world")
