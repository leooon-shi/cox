option(COW_BUILD_DOCS "Build project documentation from AsciiDoc" OFF)

if (NOT COW_BUILD_DOCS)
    message(STATUS "Documentation build disabled (COW_BUILD_DOCS=OFF)")
    return()
endif()

find_program(ASCIIDOCTOR_EXECUTABLE NAMES asciidoctor)

if (NOT ASCIIDOCTOR_EXECUTABLE)
    message(WARNING "asciidoctor not found. Documentation will not be generated.")
    return()
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/docs)

set(ASCIIDOC_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/event_io.adoc)
set(ASCIIDOC_OUTPUT ${CMAKE_BINARY_DIR}/docs/event_io.html)

add_custom_command(
    OUTPUT ${ASCIIDOC_OUTPUT}
    COMMAND ${ASCIIDOCTOR_EXECUTABLE} -o ${ASCIIDOC_OUTPUT} ${ASCIIDOC_INPUT}
    DEPENDS ${ASCIIDOC_INPUT}
    COMMENT "Generating HTML documentation from event_io.adoc"
    VERBATIM
)

add_custom_target(doc ALL DEPENDS ${ASCIIDOC_OUTPUT})