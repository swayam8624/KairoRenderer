# Apply one or more Git patches idempotently to a FetchContent checkout.
#
# A FetchContent update may revisit an already-patched source tree. Plain
# 'git apply' then fails even though the desired source state is correct.
# For each patch we therefore:
#   1. apply it if 'git apply --check' succeeds;
#   2. otherwise accept it if '--reverse --check' proves it is already applied;
#   3. if a declared legacy patch is proven to be applied, reverse that exact
#      legacy patch and apply the current patch;
#   4. fail for every other state (including partial/conflicting patches).
#
# Optional migration variables are named KAIRO_PATCH_<N>_LEGACY and correspond
# to KAIRO_PATCH_<N>. They are intentionally explicit: we never mutate an
# unknown dependency state just to make configuration continue.

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

set(_patch_index 0)
foreach(_patch IN LISTS _patches)
    math(EXPR _patch_index "${_patch_index} + 1")

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

    set(_legacy_var "KAIRO_PATCH_${_patch_index}_LEGACY")
    if(DEFINED ${_legacy_var} AND NOT "${${_legacy_var}}" STREQUAL "")
        set(_legacy_patch "${${_legacy_var}}")
        if(NOT EXISTS "${_legacy_patch}")
            message(FATAL_ERROR "Legacy patch file does not exist: ${_legacy_patch}")
        endif()

        execute_process(
            COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
                apply --reverse --check --whitespace=nowarn "${_legacy_patch}"
            RESULT_VARIABLE _legacy_is_applied
            OUTPUT_QUIET
            ERROR_QUIET)

        if(_legacy_is_applied EQUAL 0)
            message(STATUS
                "Migrating dependency patch state from legacy patch: ${_legacy_patch}")

            execute_process(
                COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
                    apply --reverse --whitespace=nowarn "${_legacy_patch}"
                RESULT_VARIABLE _legacy_reverse_result
                OUTPUT_VARIABLE _legacy_reverse_stdout
                ERROR_VARIABLE _legacy_reverse_stderr)
            if(NOT _legacy_reverse_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to reverse legacy patch '${_legacy_patch}'.\n"
                    "${_legacy_reverse_stdout}\n${_legacy_reverse_stderr}")
            endif()

            execute_process(
                COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
                    apply --check --whitespace=nowarn "${_patch}"
                RESULT_VARIABLE _migrated_can_apply
                OUTPUT_QUIET
                ERROR_QUIET)
            if(NOT _migrated_can_apply EQUAL 0)
                message(FATAL_ERROR
                    "Legacy patch was reversed, but current patch is still not applicable: ${_patch}")
            endif()

            execute_process(
                COMMAND "${KAIRO_PATCH_GIT}" -C "${KAIRO_PATCH_SOURCE_DIR}"
                    apply --whitespace=nowarn "${_patch}"
                RESULT_VARIABLE _migrated_apply_result
                OUTPUT_VARIABLE _migrated_apply_stdout
                ERROR_VARIABLE _migrated_apply_stderr)
            if(NOT _migrated_apply_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to apply current patch after legacy migration '${_patch}'.\n"
                    "${_migrated_apply_stdout}\n${_migrated_apply_stderr}")
            endif()

            message(STATUS "Applied dependency patch after legacy migration: ${_patch}")
            continue()
        endif()
    endif()

    message(FATAL_ERROR
        "Dependency patch is neither applicable nor already applied, and no declared legacy state matched: ${_patch}\n"
        "Source directory: ${KAIRO_PATCH_SOURCE_DIR}")
endforeach()
