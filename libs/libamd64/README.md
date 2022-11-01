# libamd64compilation

libamd64 is an assembler and disassembler library for the AMD64 architecture.

Because this library's intended use is for JIT code generation, don't expect it to work or even build on non-AMD64 architectures. It doesn't (yet) support all opcodes, but it supports a diverse enough subset to be able to express most programs.

## Building

libamd64 depends on phosg (https://www.github.com/fuzziqersoftware/phosg). To build and install libamd64, first build and install phosg, then run `make && sudo make install` in this directory.

## Usage

### Binaries

libamd64 includes two binaries that assemble and disassemble code (amd64asm and amd64dasm respectively). These binaries can assemble and disassemble any opcode that libamd64 itself can handle. Run them with `--help` for usage information.

### Assembling code directly

To generate code, create an `AMD64Assembler` object and call the various write functions on it. When you've generated all the code you want to, call assemble() on it, add the code to a CodeBuffer, and call it. Here's an example implementation of the FNV1A64 hash function:

    #include <libamd64/AMD64Assembler.hh>
    #include <libamd64/CodeBuffer.hh>

    AMD64Assembler as;

    as.write_mov(rax, 0xCBF29CE484222325);
    as.write_add(rsi, rdi);
    as.write_xor(rdx, rdx);
    as.write_mov(rcx, 0x00000100000001B3);
    as.write_jmp("check_end");

    as.write_label("continue");
    as.write_mov(dl, MemoryReference(rdi, 0), OperandSize::Byte);
    as.write_xor(rax, rdx);
    as.write_imul(rax, rcx);
    as.write_inc(rdi);
    as.write_label("check_end");
    as.write_cmp(rdi, rsi);
    as.write_jne("continue");

    multimap<size_t, string> compiled_labels;
    unordered_set<size_t> patch_offsets;
    string data = as.assemble(&patch_offsets, &compiled_labels);

    CodeBuffer code;
    auto fnv1a64 = reinterpret_cast<uint64_t(*)(const void*, size_t)>(
            code.append(data, &patch_offsets));

    // now we can call fnv1a64(data, size)

### Assembling code from (Intel-syntax) text

libamd64 can also assemble Intel-syntax text into executable binary. To do so, use `assemble_file(text)` (from libamd64/FileAssembler.hh). This function takes a string containing the assembly text, and returns the following structure:

    struct AssembledFile {
      std::string code;
      std::unordered_set<size_t> patch_offsets;
      std::multimap<size_t, std::string> label_offsets;
      std::vector<std::string> errors;
    };

If the `.errors` field is empty, then the code (in the `.code` field) is completely assembled and can be added to a CodeBuffer and executed.

### Disassembling binary code

To disassemble code into Intel-syntax text, call the static function `AMD64Assembler::disassemble`. To illustrate, if we extend the FNV1A64 example above to also do the following:

    string disassembly = AMD64Assembler::disassemble(
            data, reinterpret_cast<size_t>(fnv1a64), &compiled_labels);
    fprintf(stderr, "%s\n", disassembly.c_str());

Then it would output something like this (addresses will vary):

    000000010BF6C000   48 B8 25 23 22 84 E4 9C F2 CB   movabs   rax, 0xCBF29CE484222325
    000000010BF6C00A   48 01 FE                        add      rsi, rdi
    000000010BF6C00D   48 31 D2                        xor      rdx, rdx
    000000010BF6C010   48 B9 B3 01 00 00 00 01 00 00   movabs   rcx, 0x00000100000001B3
    000000010BF6C01A   EB 0C                           jmp      +0xC ; check_end
    continue:
    000000010BF6C01C   8A 17                           mov      dl, [rdi]
    000000010BF6C01E   48 31 D0                        xor      rax, rdx
    000000010BF6C021   48 0F AF C1                     imul     rax, rcx
    000000010BF6C025   48 FF C7                        inc      rdi
    check_end:
    000000010BF6C028   48 39 F7                        cmp      rdi, rsi
    000000010BF6C02B   75 EF                           jne      -0x11 ; continue
