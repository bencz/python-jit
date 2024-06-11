#pragma once

#include <stdint.h>
#include <stdlib.h>


// this structure is only here for reference. don't actually use it because it
// contains two levels of variable-size data; using it in C++ doesn't make sense
struct ExceptionBlock
{
    // pointer to the next exception block
    ExceptionBlock *next;

    // stack and frame pointers to start the except block with (should be the same
    // as when the function or try block was entered)
    void *resume_rsp;
    void *resume_rbp;

    // common object and global space pointers
    void *resume_r12;
    void *resume_r13;

    struct ExceptionBlockSpec
    {
        // start of the relevant except block's code
        const void *resume_rip;

        // class ids of exception type specified in except block. if none are given,
        // this block matches any exception, but doesn't clear r15 (this is used to
        // jump to function teardown/local destructor calls and finally blocks)
        int64_t num_classes;
        int64_t exc_class_ids[0];
    };
    ExceptionBlockSpec spec[0];
};

extern const size_t return_exception_block_size;


void raise_python_exception_with_message(ExceptionBlock *exc_block,
                                         int64_t class_id, const char *message);

void raise_python_exception_with_format(ExceptionBlock *exc_block,
                                        int64_t class_id, const char *fmt, ...);

extern "C" {

// everything in here is implemented in Exception-Assembly.s

// raise a python exception. exc_block should be the exception block passed to
// the c function, and exc should be an instance of the exception class. this
// function returns only if exc_block is nullptr, in which case it does nothing.
void raise_python_exception(ExceptionBlock *exc_block, void *exc);

// raise a python exception from pyjit-generated code. this function cannot be
// called normally; it can only be called from code generated by pyjit since
// it accepts arguments in nonstandard registers. to raise an exception from
// c/c++ code, call raise_python_exception above.
void _unwind_exception_internal();

} // extern "C"
