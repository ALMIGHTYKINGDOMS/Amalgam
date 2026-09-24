# The visual-capture manifest must describe the exact fixture registry compiled
# into the launcher that CTest is about to exercise.  This is intentionally a
# black-box CLI contract: it protects capture from a stale binary, source-text
# parser drift, or a route added to only one side of the matrix.
if(NOT DEFINED AMALGAM_LAUNCHER OR NOT DEFINED AMALGAM_MANIFEST)
    message(FATAL_ERROR "Fixture inventory test inputs are incomplete")
endif()

if(NOT EXISTS "${AMALGAM_LAUNCHER}")
    message(FATAL_ERROR "Launcher binary is missing: ${AMALGAM_LAUNCHER}")
endif()
if(NOT EXISTS "${AMALGAM_MANIFEST}")
    message(FATAL_ERROR "Visual-QA manifest is missing: ${AMALGAM_MANIFEST}")
endif()

execute_process(
    COMMAND "${AMALGAM_LAUNCHER}" --list-ui-fixtures
    RESULT_VARIABLE launcher_result
    OUTPUT_VARIABLE launcher_output
    ERROR_VARIABLE launcher_error
    TIMEOUT 10)
if(NOT launcher_result EQUAL 0)
    message(FATAL_ERROR
        "--list-ui-fixtures failed with exit ${launcher_result}: ${launcher_error}${launcher_output}")
endif()

string(JSON inventory_type ERROR_VARIABLE inventory_parse_error TYPE "${launcher_output}")
if(NOT inventory_parse_error STREQUAL "NOTFOUND" OR NOT inventory_type STREQUAL "OBJECT")
    message(FATAL_ERROR
        "--list-ui-fixtures did not return a JSON object: ${inventory_parse_error}; output=${launcher_output}")
endif()
string(JSON inventory_schema ERROR_VARIABLE inventory_schema_error GET "${launcher_output}" schemaVersion)
if(NOT inventory_schema_error STREQUAL "NOTFOUND" OR NOT inventory_schema STREQUAL "1")
    message(FATAL_ERROR "Fixture inventory has unsupported schemaVersion '${inventory_schema}'.")
endif()
string(JSON binary_count ERROR_VARIABLE binary_count_error GET "${launcher_output}" fixtureCount)
if(NOT binary_count_error STREQUAL "NOTFOUND" OR NOT binary_count MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "Fixture inventory has invalid fixtureCount '${binary_count}'.")
endif()
string(JSON binary_array_length ERROR_VARIABLE binary_array_error LENGTH "${launcher_output}" fixtures)
if(NOT binary_array_error STREQUAL "NOTFOUND" OR binary_array_length LESS 1)
    message(FATAL_ERROR "Fixture inventory must contain a non-empty fixtures array.")
endif()
if(NOT binary_count EQUAL binary_array_length)
    message(FATAL_ERROR
        "Fixture inventory declares ${binary_count} entries but returns ${binary_array_length} array elements.")
endif()

set(binary_tokens)
math(EXPR binary_last "${binary_array_length} - 1")
foreach(index RANGE 0 ${binary_last})
    string(JSON token ERROR_VARIABLE token_error GET "${launcher_output}" fixtures ${index})
    if(NOT token_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "Could not read launcher fixture at index ${index}: ${token_error}")
    endif()
    string(TOLOWER "${token}" token_lower)
    # CMake's bundled regex engine does not support interval quantifiers
    # consistently across its supported versions, so use the equivalent
    # one-or-more shape here. The launcher itself owns the length bound.
    if(NOT token STREQUAL token_lower OR NOT token MATCHES "^[a-z0-9][a-z0-9-]*$")
        message(FATAL_ERROR "Launcher fixture token '${token}' is not canonical lowercase token syntax.")
    endif()
    list(FIND binary_tokens "${token}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "Launcher fixture inventory repeats '${token}'.")
    endif()
    list(APPEND binary_tokens "${token}")
endforeach()
list(LENGTH binary_tokens binary_unique_count)
if(NOT binary_unique_count EQUAL binary_count)
    message(FATAL_ERROR
        "Launcher fixture inventory declares ${binary_count} entries but has ${binary_unique_count} unique tokens.")
endif()

file(READ "${AMALGAM_MANIFEST}" manifest_json)
string(JSON manifest_type ERROR_VARIABLE manifest_parse_error TYPE "${manifest_json}")
if(NOT manifest_parse_error STREQUAL "NOTFOUND" OR NOT manifest_type STREQUAL "OBJECT")
    message(FATAL_ERROR "Visual-QA manifest is not a JSON object: ${manifest_parse_error}")
endif()
string(JSON manifest_declared_count ERROR_VARIABLE manifest_count_error GET
       "${manifest_json}" registryFixtureTokenCount)
if(NOT manifest_count_error STREQUAL "NOTFOUND" OR
   NOT manifest_declared_count MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
        "Visual-QA manifest has invalid registryFixtureTokenCount '${manifest_declared_count}'.")
endif()
string(JSON manifest_case_count ERROR_VARIABLE manifest_cases_error LENGTH "${manifest_json}" cases)
if(NOT manifest_cases_error STREQUAL "NOTFOUND" OR manifest_case_count LESS 1)
    message(FATAL_ERROR "Visual-QA manifest must contain a non-empty cases array.")
endif()

set(manifest_tokens)
math(EXPR manifest_last "${manifest_case_count} - 1")
foreach(index RANGE 0 ${manifest_last})
    string(JSON route ERROR_VARIABLE route_error GET "${manifest_json}" cases ${index} route)
    if(NOT route_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "Manifest case ${index} has no readable route: ${route_error}")
    endif()
    string(TOLOWER "${route}" route_lower)
    string(REGEX MATCH "^([a-z0-9][a-z0-9-]*)(@(top|middle|bottom))?$" route_match "${route_lower}")
    if(NOT route_match)
        message(FATAL_ERROR
            "Manifest case ${index} route '${route}' is not a fixture token with an optional @top/@middle/@bottom suffix.")
    endif()
    set(token "${CMAKE_MATCH_1}")
    list(FIND manifest_tokens "${token}" route_token_index)
    if(route_token_index EQUAL -1)
        list(APPEND manifest_tokens "${token}")
    endif()
endforeach()
list(LENGTH manifest_tokens manifest_unique_count)
if(NOT manifest_declared_count EQUAL manifest_unique_count)
    message(FATAL_ERROR
        "Visual-QA manifest declares ${manifest_declared_count} fixture tokens but its cases resolve to ${manifest_unique_count} unique routes.")
endif()

set(binary_only)
foreach(token IN LISTS binary_tokens)
    list(FIND manifest_tokens "${token}" manifest_index)
    if(manifest_index EQUAL -1)
        list(APPEND binary_only "${token}")
    endif()
endforeach()
set(manifest_only)
foreach(token IN LISTS manifest_tokens)
    list(FIND binary_tokens "${token}" binary_index)
    if(binary_index EQUAL -1)
        list(APPEND manifest_only "${token}")
    endif()
endforeach()

if(NOT binary_count EQUAL manifest_declared_count OR binary_only OR manifest_only)
    if(binary_only)
        list(JOIN binary_only ", " binary_only_text)
    else()
        set(binary_only_text "none")
    endif()
    if(manifest_only)
        list(JOIN manifest_only ", " manifest_only_text)
    else()
        set(manifest_only_text "none")
    endif()
    message(FATAL_ERROR
        "Fixture inventory mismatch: binary=${binary_count}; manifest-declared=${manifest_declared_count}; manifest-routes=${manifest_unique_count}; binary-only=${binary_only_text}; manifest-only=${manifest_only_text}")
endif()

message(STATUS
    "fixture inventory integrity passed: ${binary_count} binary token(s) match ${manifest_unique_count} manifest route token(s)")
