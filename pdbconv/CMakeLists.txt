add_executable(pdbconv
    compression.cpp
    compression.h
    decompression.cpp
    decompression.h
    definitions.h
    main.cpp
    test.cpp
    test.h
)

target_link_libraries(pdbconv PRIVATE ynwheaders zstd::libzstd_static)
target_compile_features(pdbconv PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(pdbconv PRIVATE NOMINMAX)
endif()
