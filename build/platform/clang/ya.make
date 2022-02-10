RESOURCES_LIBRARY()
 
LICENSE(BSD-3-Clause)

OWNER(g:contrib)
 
IF (CLANG10BC AND CLANG10 AND HOST_OS_LINUX)
    DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
        CLANG # Clang 10
        sbr:1871274282 FOR LINUX
        sbr:1844539535 FOR DARWIN
    )
ELSE()
    # Remove -std=c++17 flag from LLVM_COMPILE_CXX and CLANG_EMIT_AST_CXX macro when update clang to 11
    # DEVTOOLS-7845
    DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
        CLANG # Clang 7
        sbr:707372498 FOR LINUX # In 942524429 and 958916803 llvm-link does not run.
        sbr:942053644 FOR DARWIN
        sbr:1006151596 FOR WIN32 # Generated by ./clang-windows.sh 7.1.0
    )
ENDIF()
 
END() 
