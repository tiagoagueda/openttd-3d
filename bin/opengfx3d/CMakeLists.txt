# The OpenGFX3D package: 3D models for the experimental 3D view.
# Its files are copied next to the game, where the game looks for them.

set(OPENGFX3D_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/opengfx3d.json
        ${CMAKE_CURRENT_SOURCE_DIR}/bus.glb
        ${CMAKE_CURRENT_SOURCE_DIR}/lorry.glb
)

foreach(OPENGFX3D_SOURCE_FILE IN LISTS OPENGFX3D_SOURCE_FILES)
    string(REPLACE "${CMAKE_SOURCE_DIR}/bin/" "" OPENGFX3D_SOURCE_FILE_NAME "${OPENGFX3D_SOURCE_FILE}")
    string(CONCAT OPENGFX3D_BINARY_FILE "${CMAKE_BINARY_DIR}/" "${OPENGFX3D_SOURCE_FILE_NAME}")

    add_custom_command(OUTPUT ${OPENGFX3D_BINARY_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${OPENGFX3D_SOURCE_FILE}
                    ${OPENGFX3D_BINARY_FILE}
            MAIN_DEPENDENCY ${OPENGFX3D_SOURCE_FILE}
            COMMENT "Copying ${OPENGFX3D_SOURCE_FILE_NAME}"
    )

    list(APPEND OPENGFX3D_BINARY_FILES ${OPENGFX3D_BINARY_FILE})
endforeach()

# Create a new target which copies all files of the package
add_custom_target(opengfx3d_files
        DEPENDS ${OPENGFX3D_BINARY_FILES}
)

add_dependencies(openttd
    opengfx3d_files
)
