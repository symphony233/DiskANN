if (NOT MSVC)
	message(STATUS "Setting up `make format` and `make checkformat`")
	# additional target to perform clang-format run, requires clang-format
	# get all project files
	file(GLOB_RECURSE ALL_SOURCE_FILES *.cpp *.h)

	add_custom_target(
		format
	        COMMAND /usr/bin/clang-format
        	-i
        	${ALL_SOURCE_FILES}
	)
	add_custom_target(
		checkformat
		COMMAND /usr/bin/clang-format
		--dry-run
		${ALL_SOURCE_FILES}
	)
endif()
