if (GINKGO_HAVE_PAPI_SDE)
	add_executable(papi-logging papi-logging.cpp)
	target_link_libraries(papi-logging ginkgo PAPI::PAPI)
	target_include_directories(papi-logging PRIVATE ${PROJECT_SOURCE_DIR})
	configure_file(data/A.mtx data/A.mtx COPYONLY)
	configure_file(data/b.mtx data/b.mtx COPYONLY)
	configure_file(data/x0.mtx data/x0.mtx COPYONLY)
endif()
