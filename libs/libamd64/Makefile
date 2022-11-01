ifeq ($(shell uname -s),Darwin)
	INSTALL_DIR=/opt/local
	CXXFLAGS +=  -DMACOSX -mmacosx-version-min=10.11
else
	INSTALL_DIR=/usr/local
	CXXFLAGS +=  -DLINUX
endif

CXX=g++ -fPIC
CXXLD=g++
OBJECTS=CodeBuffer.o AMD64Assembler.o FileAssembler.o
CXXFLAGS=-O2 -Wall -std=c++20 -I$(INSTALL_DIR)/include
LDFLAGS=-L$(INSTALL_DIR)/lib
LIBS=-lphosg -lpthread

all: libamd64.a amd64dasm amd64asm test

install: libamd64.a amd64asm amd64dasm
	mkdir -p $(INSTALL_DIR)/include/libamd64
	cp libamd64.a $(INSTALL_DIR)/lib/
	cp -r *.hh $(INSTALL_DIR)/include/libamd64/
	cp amd64asm $(INSTALL_DIR)/bin/
	cp amd64dasm $(INSTALL_DIR)/bin/

libamd64.a: $(OBJECTS)
	rm -f libamd64.a
	ar rcs libamd64.a $(OBJECTS)

AMD64AssemblerTest: CodeBuffer.o AMD64Assembler.o AMD64AssemblerTest.o
	$(CXXLD) $(LDFLAGS) -o AMD64AssemblerTest $^ $(LIBS)

amd64dasm: AMD64Assembler.o amd64dasm.o
	$(CXXLD) $(LDFLAGS) -o amd64dasm $^ $(LIBS)

amd64asm: AMD64Assembler.o FileAssembler.o amd64asm.o
	$(CXXLD) $(LDFLAGS) -o amd64asm $^ $(LIBS)

test: AMD64AssemblerTest
	./AMD64AssemblerTest

clean:
	rm -rf *.dSYM *.o gmon.out libamd64.a amd64asm amd64dasm *Test

.PHONY: clean