# Apply one or more Git patches idempotently to a FetchContent checkout.
#
# A FetchContent update may revisit an already-patched source tree. Plain
# 'git apply' then fails even though the desired source state is correct.
# For each patch we therefore:
#   1. apply it if 'git apply --check' succeeds;
#   2. otherwise accept it only if '--reverse --check' proves it is already
#      applied;
#   3. fail for every other state (including partial/conflicting patches).

foreach(required IN ITEMS
    KAIRO_PATCH_GIT
    KAIRO_PATCH_SOURCE_DIR
    KAIRO_PATCH_1)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required patch variable: ${required}")
    endif()
endforeach()

set(_patches "${KAIRO_PATCH_1}")
if(DEFINED KAIRO_PATCH_2 AND NOT "${KAIRO_PATCH_2}" STREQUAL "")
    list(APPEND _patches "${KAIRO_PATCH_2}")
endif()

foreach(_patch IN LISTS _patches)
    if(NOT EXISTS "${_patch}")
        message(FATAL_ERROR "Patch file does not exist: ${_patch}")
    endif()

    execute_process(
        COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
            apply --check --whitespace=nowarn "${_patch}"
        RESULT_VARIABLE _can_apply
        OUTPUT_QUIET
        ERROR_QUIET)

    if(_can_apply EQUAL 0)
        execute_process(
            COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
                apply --whitespace=nowarn "${_patch}"
            RESULT_VARIABLE _apply_result
            OUTPUT_VARIABLE _apply_stdout
            ERROR_VARIABLE _apply_stderr)

        if(NOT _apply_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply patch '${_patch}'.\n"
                "${_apply_stdout}\n${_apply_stderr}")
        endif()

        message(STATUS "Applied dependency patch: ${_patch}")
        continue()
    endif()

    execute_process(
        COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
            apply --reverse --check --whitespace=nowarn "${_patch}"
        RESULT_VARIABLE _already_applied
        OUTPUT_QUIET
        ERROR_QUIET)

    if(_already_applied EQUAL 0)
        message(STATUS "Dependency patch already applied: ${_patch}")
        continue()
    endif()

    message(FATAL_ERROR
        "Dependency patch is neither applicable nor already applied: ${_patch}\n"
        "Source directory: ${KAIRO_PATCH_SOURCE_DIR}")
endforeach()
