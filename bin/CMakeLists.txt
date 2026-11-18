add_subdirectory(ai)
add_subdirectory(game)

# The OpenGFX3D package of 3D models for the experimental 3D view, a git submodule in opengfx3d.
# Its index, models and textures are copied next to the game, where the game looks for them.
if(VIEWPORT3D_ENABLED AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/opengfx3d/opengfx3d.json)
    file(GLOB OPENGFX3D_SOURCE_FILES CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/opengfx3d/opengfx3d.json
            ${CMAKE_CURRENT_SOURCE_DIR}/opengfx3d/*.glb
            ${CMAKE_CURRENT_SOURCE_DIR}/opengfx3d/*.png
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

    add_custom_target(opengfx3d_files
            DEPENDS ${OPENGFX3D_BINARY_FILES}
    )

    add_dependencies(openttd
        opengfx3d_files
    )
elseif(VIEWPORT3D_ENABLED)
    message(WARNING "The OpenGFX3D package is missing; run 'git submodule update --init' to get the 3D models.")
endif()
