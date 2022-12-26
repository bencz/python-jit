cmake_minimum_required(VERSION 3.16)
project(pyjit CXX ASM)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_COMPILER clang++)
SET(CMAKE_ASM_COMPILER clang++)

set(CMAKE_CXX_FLAGS "-DLINUX -fPIC -Wno-strict-aliasing -Wno-unused-result -Wno-overflow")

include_directories("./libs")

add_executable(pyjit
        # phosg
        libs/phosg/Encoding.cc
        libs/phosg/Filesystem.cc
        libs/phosg/Hash.cc
        libs/phosg/Image.cc
        libs/phosg/JSON.cc
        libs/phosg/Network.cc
        libs/phosg/Process.cc
        libs/phosg/Random.cc
        libs/phosg/Strings.cc
        libs/phosg/Time.cc
        libs/phosg/Tools.cc
        libs/phosg/UnitTest.cc

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
        source/Modules/__pyjit__.cc
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
