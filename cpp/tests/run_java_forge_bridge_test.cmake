if(NOT DEFINED ENV{AMALGAM_JAVAC} OR NOT DEFINED ENV{AMALGAM_JAVA})
    message(FATAL_ERROR "AMALGAM_JAVAC / AMALGAM_JAVA not set")
endif()

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../../java-forge/src/main/java/amalgam/bridge" COMMON_SRC)
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../../java/common/src/test/java/amalgam/bridge" TEST_SRC)
set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/java_forge_bridge_test")
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

set(SOURCES
    "${COMMON_SRC}/Actions.java"
    "${COMMON_SRC}/NativeBridge.java"
    "${COMMON_SRC}/TelemetryCodec.java"
    "${TEST_SRC}/ProtocolTest.java"
)
execute_process(
    COMMAND "$ENV{AMALGAM_JAVAC}" -d "${OUT_DIR}" ${SOURCES}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "javac failed: ${err}\n${out}")
endif()
execute_process(
    COMMAND "$ENV{AMALGAM_JAVA}" -cp "${OUT_DIR}" amalgam.bridge.ProtocolTest
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "java test failed (rc=${rc}): ${err}\n${out}")
endif()
message(STATUS "forge java bridge test passed")
