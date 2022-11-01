cmake_minimum_required(VERSION 3.23)
project(pyjit CXX ASM)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
SET(CMAKE_ASM_COMPILER clang++)

set(CMAKE_C_FLAGS "-DLINUX")
set(CMAKE_CXX_FLAGS "-DLINUX")

include_directories("/libs/phosg/src")
include_directories("/libs/libamd64")

set_property(SOURCE libs/phosg/src/Encoding.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Filesystem.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Hash.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Image.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/JSON.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Network.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Process.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Random.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Strings.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Time.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/Tools.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)
set_property(SOURCE libs/phosg/src/UnitTest.cc APPEND PROPERTY COMPILE_OPTIONS -Wno-strict-aliasing -Wno-unused-result -Wno-overflow)

set_property(SOURCE libs/libamd64/AMD64Assembler.cc APPEND PROPERTY COMPILE_OPTIONS -fPIC)
set_property(SOURCE libs/libamd64/CodeBuffer.cc APPEND PROPERTY COMPILE_OPTIONS -fPIC)
set_property(SOURCE libs/libamd64/FileAssembler.cc APPEND PROPERTY COMPILE_OPTIONS -fPIC)


add_executable(pyjit
        # phosg
        libs/phosg/src/Encoding.cc
        libs/phosg/src/Filesystem.cc
        libs/phosg/src/Hash.cc
        libs/phosg/src/Image.cc
        libs/phosg/src/JSON.cc
        libs/phosg/src/Network.cc
        libs/phosg/src/Process.cc
        libs/phosg/src/Random.cc
        libs/phosg/src/Strings.cc
        libs/phosg/src/Time.cc
        libs/phosg/src/Tools.cc
        libs/phosg/src/UnitTest.cc

        # libamd64
        libs/libamd64/AMD64Assembler.cc
        libs/libamd64/CodeBuffer.cc
        libs/libamd64/FileAssembler.cc

        # pyjit
        source/Main.cc
        source/Debug.cc 
        source/AST/SourceFile.cc 
        source/AST/PythonLexer.cc 
        source/AST/PythonParser.cc 
        source/AST/PythonASTNodes.cc 
        source/AST/PythonASTVisitor.cc 
        source/Types/Reference.cc 
        source/Types/Strings.cc 
        source/Types/Format.cc 
        source/Types/Tuple.cc 
        source/Types/List.cc 
        source/Types/Dictionary.cc 
        source/Types/Instance.cc 
        source/Modules/builtins.cc 
        source/Modules/__nemesys__.cc 
        source/Modules/sys.cc 
        source/Modules/math.cc 
        source/Modules/posix.cc 
        source/Modules/errno.cc 
        source/Modules/time.cc 
        source/Environment/Operators.cc 
        source/Environment/Value.cc
        source/Compiler/Compile.cc 
        source/Compiler/Compile-Assembly.s
        source/Compiler/Contexts.cc 
        source/Compiler/BuiltinFunctions.cc 
        source/Compiler/CommonObjects.cc 
        source/Compiler/Exception.cc 
        source/Compiler/Exception-Assembly.s
        source/Compiler/AnnotationVisitor.cc
        source/Compiler/AnalysisVisitor.cc
        source/Compiler/CompilationVisitor.cc)

target_link_libraries(pyjit -lpthread)