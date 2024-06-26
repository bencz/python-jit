#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <libamd64/CodeBuffer.hh>

#include "../AST/PythonASTNodes.hh"
#include "../AST/SourceFile.hh"
#include "../Types/Strings.hh"


class compile_error : public std::runtime_error
{
public:
    explicit compile_error(const std::string &what, ssize_t where = -1);
    ~compile_error() override = default;

    ssize_t where;
};


struct ModuleContext;
struct FunctionContext;
struct GlobalContext;

struct BuiltinFragmentDefinition
{
    std::vector<Value> arg_types;
    Value return_type;
    const void *compiled;

    BuiltinFragmentDefinition(const std::vector<Value> &arg_types,
                              Value return_type, const void *compiled);
};

struct BuiltinFunctionDefinition
{
    const char *name;
    std::vector<BuiltinFragmentDefinition> fragments;
    bool pass_exception_block;

    BuiltinFunctionDefinition(const char *name,
                              const std::vector<Value> &arg_types, Value return_type,
                              const void *compiled, bool pass_exception_blocky);

    BuiltinFunctionDefinition(const char *name,
                              const std::vector<BuiltinFragmentDefinition> &fragments,
                              bool pass_exception_block);
};

struct BuiltinClassDefinition
{
    const char *name;
    std::map<std::string, Value> attributes;
    std::vector<BuiltinFunctionDefinition> methods;
    const void *destructor;

    BuiltinClassDefinition(const char *name,
                           const std::map<std::string, Value> &attributes,
                           const std::vector<BuiltinFunctionDefinition> &methods,
                           const void *destructor);
};

struct Fragment
{
    FunctionContext *function;
    size_t index;

    std::vector<Value> arg_types;
    Value return_type;

    std::vector<ssize_t> call_split_offsets;
    std::vector<std::string> call_split_labels;
    const void *compiled{};
    std::multimap<size_t, std::string> compiled_labels;

    Fragment() = delete;

    // dynamic function constructor
    Fragment(FunctionContext *fn, size_t index,
             const std::vector<Value> &arg_types);

    // builtin function constructor
    Fragment(FunctionContext *fn, size_t index,
             const std::vector<Value> &arg_types, Value return_type,
             const void *compiled);

    void resolve_call_split_labels();
};


struct ClassContext
{
    struct ClassAttribute
    {
        std::string name;
        Value value;

        ClassAttribute(const std::string &name, Value value);
    };

    // ClassContext objects are created during the annotation phase of importing,
    // so nothing here is technically valid until the owning module is in the
    // Annotated phase or later

    // the following are valid when the owning module is Annotated or later
    ModuleContext *module; // nullptr for built-in class definitions
    int64_t id; // note that __init__ has the same ID
    std::string name;
    ASTNode *ast_root;
    int64_t parent_class_id{}; // can be zero

    // this field's keys are valid when the owning module is Annotated or later,
    // but the values aren't valid until Analyzed or later
    std::vector<ClassAttribute> attributes;

    std::unordered_map<std::string, size_t> attribute_indexes; // valid when Annotated

    // the following are valid when the owning module is Imported or later
    const void *destructor; // generated when class def is visited by CompilationVisitor

    ClassContext(ModuleContext *module, int64_t id);

    void populate_dynamic_attributes();

    int64_t attribute_count() const;

    int64_t instance_size() const;

    int64_t offset_for_attribute(const char *attribute) const;

    static int64_t offset_for_attribute(size_t index) ;
};

struct FunctionContext
{
    // FunctionContext objects are created during the annotation phase of
    // importing, so nothing here is technically valid until the owning module is
    // in the Annotated phase or later

    // the following are valid when the owning module is Annotated or later
    ModuleContext *module; // nullptr for built-in functions
    int64_t id;
    int64_t class_id; // id of class that this function is a method of; 0 for free functions
    std::string name;
    ASTNode *ast_root; // nullptr for built-in functions

    struct Argument
    {
        std::string name;
        Value default_value;
        std::shared_ptr<TypeAnnotation> type_annotation;
    };
    std::vector<Argument> args; // note: default values not valid until Analyzed
    std::string varargs_name;
    std::string varkwargs_name;

    int64_t num_splits;
    bool pass_exception_block;

    std::unordered_set<std::string> explicit_globals;

    // this field's keys are valid when the owning module is Annotated or later,
    // but the values aren't valid until Analyzed or later
    std::map<std::string, Value> locals;

    // the following are valid when the owning module is Analyzed or later
    std::unordered_set<Value> return_types;
    Value annotated_return_type;

    // the following are valid when the owning module is Imported or later
    std::vector<Fragment> fragments;

    // constructor for dynamic functions (defined in .py files)
    FunctionContext(ModuleContext *module, int64_t id);

    // constructor for builtin functions
    FunctionContext(ModuleContext *module, int64_t id, const char *name,
                    const std::vector<BuiltinFragmentDefinition> &fragments,
                    bool pass_exception_block);

    bool is_class_init() const;

    bool is_builtin() const;

    // gets the index of the fragment that satisfies the given call args, or -1 if
    // no appropriate fragment exists
    int64_t fragment_index_for_call_args(const std::vector<Value> &arg_types) const;

    // gets the appropriate type for the given annotation
    Value type_for_annotation(std::shared_ptr<const TypeAnnotation> type_annotation) const;
};


struct ModuleContext
{
    enum class Phase
    {
        Initial = 0, // nothing done yet; only source file loaded
        Parsed,      // AST exists
        Annotated,   // function/class IDs assigned and names collected
        Analyzed,    // types inferred
        Imported,    // root scope has been compiled and executed
    };

    // the following are always valid
    GlobalContext *global;
    Phase phase;
    std::string name;
    std::shared_ptr<SourceFile> source; // nullptr for built-in modules

    // the following are valid in the Parsed phase and later
    std::shared_ptr<ModuleStatement> ast_root; // nullptr for built-in modules

    // the following are valid in the Annotated phase and later
    struct GlobalVariable
    {
        Value value;
        size_t index;
        int64_t flags;

        enum Flag
        {
            Mutable = 0x01,
            StaticInitialize = 0x02,
        };

        GlobalVariable(const Value &value, size_t index, int64_t flags);
    };

    std::map<std::string, GlobalVariable> global_variables; // values invalid until Analyzed
    void *global_space;

    int64_t root_fragment_num_splits;
    Fragment root_fragment;

    int64_t compiled_size; // size of all compiled blocks (root scope, functions) in this module

    // constructor for imported modules. starts at Initial phase
    ModuleContext(GlobalContext *global, const std::string &name,
                  const std::string &filename_or_code, bool is_code = false);

    // constructor for built-in modules. starts at Imported phase
    ModuleContext(GlobalContext *global, const std::string &name,
                  const std::map<std::string, Value> &globals);

    ~ModuleContext();

    bool create_global_variable(const std::string &name, const Value &v,
                                bool is_mutable, bool static_initialize = true);

    int64_t create_builtin_function(BuiltinFunctionDefinition &def);

    int64_t create_builtin_class(BuiltinClassDefinition &def);

    bool is_builtin() const;
};


struct GlobalContext
{
    CodeBuffer code;

    std::unordered_map<std::string, std::shared_ptr<ModuleContext>> modules;
    std::shared_ptr<ModuleContext> builtins_module;
    std::vector<std::string> import_paths;

    std::unordered_map<std::string, BytesObject *> bytes_constants;
    std::unordered_map<std::wstring, UnicodeObject *> unicode_constants;

    std::unordered_set<std::string> scopes_in_progress;

    std::atomic<int64_t> next_user_function_id; // starts at 1 and increases
    std::atomic<int64_t> next_builtin_function_id; // starts at -1 and decreases

    std::unordered_map<int64_t, FunctionContext> function_id_to_context;
    std::unordered_map<int64_t, ClassContext> class_id_to_context;

    int64_t AssertionError_class_id;
    int64_t IndexError_class_id;
    int64_t KeyError_class_id;
    int64_t OSError_class_id;
    int64_t PyJitCompilerError_class_id;
    int64_t TypeError_class_id;
    int64_t ValueError_class_id;

    int64_t BytesObject_class_id;
    int64_t UnicodeObject_class_id;
    int64_t DictObject_class_id;
    int64_t ListObject_class_id;
    int64_t TupleObject_class_id;
    int64_t SetObject_class_id;

    struct UnresolvedFunctionCall
    {
        int64_t callee_function_id;
        std::vector<Value> arg_types;

        ModuleContext *caller_module;
        int64_t caller_function_id;
        int64_t caller_fragment_index;
        int64_t caller_split_id;

        UnresolvedFunctionCall(int64_t callee_function_id,
                               const std::vector<Value> &arg_types, ModuleContext *caller_module,
                               int64_t caller_function_id, int64_t caller_fragment_index,
                               int64_t caller_split_id);

        std::string str() const;
    };

    // TODO: these should be in the fragment, not in the global context, so we can
    // clean them up when a fragment is recompiled
    std::unordered_map<int64_t, UnresolvedFunctionCall> unresolved_callsites;
    std::atomic<int64_t> next_callsite_token;


    explicit GlobalContext(const std::vector<std::string> &import_paths);
    ~GlobalContext();

    void print_compile_error(FILE *stream, const ModuleContext *module,
                             const compile_error &e);

    std::shared_ptr<ModuleContext> get_or_create_module(
            const std::string &module_name, const std::string &filename = "",
            bool filename_is_code = false);

    std::string find_source_file(const std::string &module_name);

    FunctionContext *context_for_function(int64_t function_id,
                                          ModuleContext *module_for_create = nullptr);

    ClassContext *context_for_class(int64_t class_id,
                                    ModuleContext *module_for_create = nullptr);

    const BytesObject *get_or_create_constant(const std::string &s,
                                              bool use_shared_constants = true);

    const UnicodeObject *get_or_create_constant(const std::wstring &s,
                                                bool use_shared_constants = true);

    int64_t match_value_to_type(const Value &expected_type, const Value &value);

    int64_t match_values_to_types(const std::vector<Value> &fn_arg_types,
                                  const std::vector<Value> &arg_types);

    Value static_attribute_lookup(ModuleContext *module, const std::string &name);

    Value type_for_annotation(ModuleContext *module,
                              std::shared_ptr<TypeAnnotation> type_annotation);
};
