if(NOT DEFINED GENERATED_INIT OR NOT EXISTS "${GENERATED_INIT}")
    message(FATAL_ERROR
        "Skate 3 generated function table is missing. Build the generate-all target, "
        "then reconfigure and build again.")
endif()

file(READ "${GENERATED_INIT}" SKATE3_GENERATED_INIT_CONTENT)
string(FIND "${SKATE3_GENERATED_INIT_CONTENT}" "0x829921F0" SKATE3_TU_ENTRY_OFFSET)
if(SKATE3_TU_ENTRY_OFFSET EQUAL -1)
    message(FATAL_ERROR
        "Skate 3 TU3 is enabled, but generated/skate3_init.cpp contains the retail-only "
        "function table. Build the generate-all target, then reconfigure and build again.")
endif()
