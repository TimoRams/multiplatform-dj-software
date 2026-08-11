# Production source ownership lives with the corresponding domain. These
# directories add files and resources to the already-created BrockDJ target.
add_subdirectory(analysis)
add_subdirectory(app)
add_subdirectory(audio)
add_subdirectory(controllers)
add_subdirectory(deck)
add_subdirectory(domain)
add_subdirectory(fx)
add_subdirectory(library)
add_subdirectory(link)
add_subdirectory(platform)
add_subdirectory(qml)
add_subdirectory(waveform)

# The root owns qt_add_qml_module(), so expose the domain-maintained file list
# exactly one scope upward.
set(BROCKDJ_QML_FILES ${BROCKDJ_QML_FILES} PARENT_SCOPE)
