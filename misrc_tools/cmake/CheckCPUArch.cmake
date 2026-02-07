macro(_CHECK_CPU_ARCH ARCH ARCH_DEFINES VARIABLE)
    if(NOT DEFINED HAVE_${VARIABLE})
        message(STATUS "Check CPU architecture is ${ARCH}")
        set(CHECK_CPU_ARCH_DEFINES ${ARCH_DEFINES})

        configure_file(
            ${CMAKE_CURRENT_LIST_DIR}/CheckCPUArch.c.in
            ${CMAKE_CURRENT_BINARY_DIR}/CheckCPUArch.c
            @ONLY
        )

        try_compile(
            HAVE_${VARIABLE}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/CheckCPUArch.c
        )

        if(HAVE_${VARIABLE})
            message(STATUS "Check CPU architecture is ${ARCH} - yes")
            set(${VARIABLE} 1 CACHE INTERNAL "Result of CHECK_CPU_ARCH" FORCE)
        else()
            message(STATUS "Check CPU architecture is ${ARCH} - no")
        endif()
    endif()
endmacro()
