add_lib (tester SOURCES tester.c)

if (BUILD_STATIC)
	set_target_properties (elektra-tester-objects PROPERTIES COMPILE_DEFINITIONS "ELEKTRA_STATIC")
endif ()
