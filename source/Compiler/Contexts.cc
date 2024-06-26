#include "Contexts.hh"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

#include "../Debug.hh"
#include "../AST/PythonLexer.hh"
#include "../Types/Instance.hh"
#include "BuiltinFunctions.hh"

using namespace std;


compile_error::compile_error(const string &what, ssize_t where) :
        runtime_error(what), where(where)
{}


BuiltinFragmentDefinition::BuiltinFragmentDefinition(
        const vector<Value> &arg_types, Value return_type,
        const void *compiled) : arg_types(arg_types), return_type(return_type),
                                compiled(compiled)
{}

BuiltinFunctionDefinition::BuiltinFunctionDefinition(const char *name,
                                                     const std::vector<Value> &arg_types, Value return_type,
                                                     const void *compiled, bool pass_exception_block) :
        name(name), fragments({{arg_types, return_type, compiled}}),
        pass_exception_block(pass_exception_block)
{}

BuiltinFunctionDefinition::BuiltinFunctionDefinition(const char *name,
                                                     const std::vector<BuiltinFragmentDefinition> &fragments,
                                                     bool pass_exception_block) : name(name),
                                                                                  fragments(fragments),
                                                                                  pass_exception_block(
                                                                                          pass_exception_block)
{}

BuiltinClassDefinition::BuiltinClassDefinition(const char *name,
                                               const std::map<std::string, Value> &attributes,
                                               const std::vector<BuiltinFunctionDefinition> &methods,
                                               const void *destructor) : name(name), attributes(attributes),
                                                                         methods(methods), destructor(destructor)
{}


Fragment::Fragment(FunctionContext *fn, size_t index,
                   const std::vector<Value> &arg_types) : function(fn), index(index),
                                                          arg_types(arg_types)
{}

Fragment::Fragment(FunctionContext *fn, size_t index,
                   const std::vector<Value> &arg_types, Value return_type,
                   const void *compiled) : function(fn), index(index),
                                           arg_types(arg_types), return_type(return_type), compiled(compiled)
{}

void Fragment::resolve_call_split_labels()
{
    unordered_map<string, size_t> label_to_index;
    for (size_t x = 0; x < this->call_split_labels.size(); x++)
    {
        const string &label = this->call_split_labels[x];

        // the label can be missing if the compiler never encountered it due to an
        // earlier split; just skip it
        if (label.empty())
        {
            continue;
        }

        if (!label_to_index.emplace(label, x).second)
        {
            throw compile_error("duplicate split label: " + label);
        }
    }

    for (const auto &it: this->compiled_labels)
    {
        try
        {
            this->call_split_offsets[label_to_index.at(it.second)] = it.first;
        } catch (const out_of_range &)
        {}
    }
}


ClassContext::ClassAttribute::ClassAttribute(const std::string &name,
                                             Value value) : name(name), value(value)
{}


ClassContext::ClassContext(ModuleContext *module, int64_t id) : module(module),
                                                                id(id), ast_root(nullptr), destructor(nullptr)
{}

int64_t ClassContext::attribute_count() const
{
    return this->attributes.size();
}

int64_t ClassContext::instance_size() const
{
    return sizeof(int64_t) * this->attribute_count() + sizeof(InstanceObject);
}

int64_t ClassContext::offset_for_attribute(const char *attribute) const
{
    return this->offset_for_attribute(this->attribute_indexes.at(attribute));
}

int64_t ClassContext::offset_for_attribute(size_t index)
{
    // attributes are stored at [instance + 8 * which + attribute_start_offset]
    return (sizeof(int64_t) * index) + sizeof(InstanceObject);
}


FunctionContext::FunctionContext(ModuleContext *module, int64_t id) :
        module(module), id(id), class_id(0), ast_root(nullptr), num_splits(0),
        pass_exception_block(false)
{}

FunctionContext::FunctionContext(ModuleContext *module, int64_t id,
                                 const char *name, const vector<BuiltinFragmentDefinition> &fragments,
                                 bool pass_exception_block) : module(module), id(id), class_id(0),
                                                              name(name), ast_root(nullptr), num_splits(0),
                                                              pass_exception_block(pass_exception_block)
{

    // populate the arguments from the first fragment definition
    for (const auto &arg: fragments[0].arg_types)
    {
        this->args.emplace_back();
        if (arg.type == ValueType::Indeterminate)
        {
            throw invalid_argument("builtin functions must have known argument types");
        }
        else if (arg.value_known)
        {
            this->args.back().default_value = arg;
        }
    }

    // now merge all the fragment argument definitions together
    for (const auto &fragment_def: fragments)
    {
        if (fragment_def.arg_types.size() != this->args.size())
        {
            throw invalid_argument("all fragments must take the same number of arguments");
        }

        for (size_t z = 0; z < fragment_def.arg_types.size(); z++)
        {
            const auto &fragment_arg = fragment_def.arg_types[z];
            if (fragment_arg.type == ValueType::Indeterminate)
            {
                throw invalid_argument("builtin functions must have known argument types");
            }
            else if (fragment_arg.value_known && (this->args[z].default_value != fragment_arg))
            {
                throw invalid_argument("all fragments must have the same default values");
            }
        }
    }

    // finally, create the fragments
    for (const auto &fragment_def: fragments)
    {
        this->return_types.emplace(fragment_def.return_type);
        this->fragments.emplace_back(this, this->fragments.size(),
                                     fragment_def.arg_types, fragment_def.return_type,
                                     fragment_def.compiled);
    }
}

bool FunctionContext::is_class_init() const
{
    return this->id == this->class_id;
}

bool FunctionContext::is_builtin() const
{
    return !this->ast_root;
}

int64_t FunctionContext::fragment_index_for_call_args(
        const vector<Value> &arg_types) const
{
    // go through the existing fragments and see if there are any that can satisfy
    // this call. if there are multiple matches, choose the most specific one (
    // the one that has the fewest Indeterminate substitutions)
    // TODO: this is linear in the number of fragments. make it faster somehow
    int64_t fragment_index = -1;
    int64_t best_match_score = -1;
    for (size_t x = 0; x < this->fragments.size(); x++)
    {
        auto &fragment = this->fragments[x];

        int64_t score = this->module->global->match_values_to_types(
                fragment.arg_types, arg_types);
        if (score < 0)
        {
            continue; // not a match
        }

        if ((best_match_score < 0) || (score < best_match_score))
        {
            fragment_index = x;
            best_match_score = score;
        }
    }

    return fragment_index;
}


ModuleContext::ModuleContext(GlobalContext *global, const string &name,
                             const string &filename, bool is_code) : global(global),
                                                                     phase(Phase::Initial), name(name),
                                                                     source(new SourceFile(filename, is_code)),
                                                                     global_space(nullptr),
                                                                     root_fragment_num_splits(0),
                                                                     root_fragment(nullptr, -1, {}), compiled_size(0)
{
    // TODO: using unescape_unicode is a stupid hack, but these strings can't
    // contain backslashes anyway (right? ...right?)
    this->create_global_variable("__name__", Value(ValueType::Unicode, unescape_unicode(name)), false);
    if (is_code)
    {
        this->create_global_variable("__file__", Value(ValueType::Unicode, L"__main__"), false);
    }
    else
    {
        this->create_global_variable("__file__", Value(ValueType::Unicode, unescape_unicode(realpath(filename))), false);
    }
}

ModuleContext::ModuleContext(GlobalContext *global, const string &name,
                             const map<string, Value> &globals) : global(global), phase(Phase::Imported),
                                                                  name(name), source(nullptr), ast_root(nullptr),
                                                                  global_space(nullptr),
                                                                  root_fragment_num_splits(0),
                                                                  root_fragment(nullptr, -1, {}), compiled_size(0)
{
    this->create_global_variable("__name__", Value(ValueType::Unicode, unescape_unicode(name)), false);
    this->create_global_variable("__file__", Value(ValueType::Unicode, L"__main__"), false);

    for (const auto &it: globals)
    {
        this->create_global_variable(it.first, it.second, false);
    }
}

ModuleContext::~ModuleContext()
{
    if (this->global_space)
    {
        free(this->global_space);
    }
}

bool ModuleContext::create_global_variable(const string &name, const Value &v,
                                           bool is_mutable, bool static_initialize)
{
    int64_t flags = (is_mutable ? GlobalVariable::Flag::Mutable : 0) |
                    (static_initialize ? GlobalVariable::Flag::StaticInitialize : 0);
    return this->global_variables.emplace(piecewise_construct, forward_as_tuple(name),
                                          forward_as_tuple(v, this->global_variables.size(), flags)).second;
}

int64_t ModuleContext::create_builtin_function(BuiltinFunctionDefinition &def)
{
    // the context already exists; all we have to do is assign a function id and
    // make a name in the current module
    int64_t function_id = this->global->next_builtin_function_id--;
    this->global->function_id_to_context.emplace(piecewise_construct,
                                                 forward_as_tuple(function_id), forward_as_tuple(this, function_id,
                                                                                                 def.name,
                                                                                                 def.fragments,
                                                                                                 def.pass_exception_block));

    // register the function in the module's global namespace
    this->create_global_variable(def.name, Value(ValueType::Function, function_id), false);
    return function_id;
}

int64_t ModuleContext::create_builtin_class(BuiltinClassDefinition &def)
{
    int64_t class_id = this->global->next_builtin_function_id--;

    // create and register the class context
    // TODO: define a ClassContext constructor that will do all this
    ClassContext &cls = this->global->class_id_to_context.emplace(piecewise_construct,
                                                                  forward_as_tuple(class_id),
                                                                  forward_as_tuple(nullptr, class_id)).first->second;
    cls.destructor = def.destructor;
    cls.name = def.name;
    cls.ast_root = nullptr;
    for (const auto &it: def.attributes)
    {
        cls.attribute_indexes.emplace(it.first, cls.attributes.size());
        cls.attributes.emplace_back(it.first, it.second);
    }

    // built-in types like Bytes, Unicode, List, Tuple, Set, and Dict don't take
    // Instance as the first argument (instead they take their corresponding
    // built-in types), so allow those if the class being defined is one of those
    // TODO: it's bad that we do this using the class name; find a better way
    // TODO: also extension type refs don't work; this is why tuple is missing
    static const Value Extension0(ValueType::ExtensionTypeReference, static_cast<int64_t>(0));
    static const Value Extension1(ValueType::ExtensionTypeReference, static_cast<int64_t>(1));
    static const Value List_Any(ValueType::List, vector<Value>({Value()}));
    static const Value List_Same(ValueType::List, vector<Value>({Extension0}));
    static const Value Set_Any(ValueType::Set, vector<Value>({Value()}));
    static const Value Set_Same(ValueType::Set, vector<Value>({Extension0}));
    static const Value Dict_Any(ValueType::Dict, vector<Value>({Value(), Value()}));
    static const Value Dict_Same(ValueType::Dict, vector<Value>({Extension0, Extension1}));
    static const unordered_map<string, unordered_set<Value>> name_to_self_types({
                                                                                        {"bytes",   {Value(
                                                                                                ValueType::Bytes)}},
                                                                                        {"unicode", {Value(
                                                                                                ValueType::Unicode)}},
                                                                                        {"list",    {List_Any, List_Same}},
                                                                                        {"set",     {Set_Any,  Set_Same}},
                                                                                        {"dict",    {Dict_Any, Dict_Same}},
                                                                                });
    unordered_set<Value> self_types({{ValueType::Instance, static_cast<int64_t>(0), nullptr}});
    try
    {
        self_types = name_to_self_types.at(def.name);
    } catch (const out_of_range &)
    {}

    // register the methods
    for (auto &method_def: def.methods)
    {
        // __del__ must not be given in the methods; it must already be compiled
        if (!strcmp(method_def.name, "__del__"))
        {
            throw logic_error(string_printf("%s defines __del__ in methods, not precompiled",
                                            def.name));
        }

        // patch all of the fragment definitions to include the correct class
        // instance type as the first argument. they should already have an Instance
        // argument first, but with a missing class_id - the caller doesn't know the
        // class id when they call create_builtin_class
        for (auto &frag_def: method_def.fragments)
        {
            if (frag_def.arg_types.empty())
            {
                throw logic_error(string_printf("%s.%s must take the class instance as an argument",
                                                def.name, method_def.name));
            }

            if (!self_types.count(frag_def.arg_types[0]))
            {
                string type_str = frag_def.arg_types[0].str();
                throw logic_error(string_printf("%s.%s cannot take %s as the first argument",
                                                def.name, method_def.name, type_str.c_str()));
            }
            if (frag_def.arg_types[0].type == ValueType::Instance)
            {
                frag_def.arg_types[0].class_id = class_id;
            }
        }

        // __init__ has some special behaviors
        int64_t function_id;
        if (!strcmp(method_def.name, "__init__"))
        {
            // if it's __init__, the return type must be the class instance, not None
            for (auto &frag_def: method_def.fragments)
            {
                if (frag_def.return_type != Value(ValueType::Instance, static_cast<int64_t>(0), nullptr))
                {
                    throw logic_error(string_printf("%s.__init__ must return the class instance",
                                                    def.name));
                }
                frag_def.return_type.class_id = class_id;
            }

            // __init__'s function id is the same as the class id
            function_id = class_id;

        }
        else
        {
            // all other functions have unique function_ids
            function_id = this->global->next_builtin_function_id--;
        }

        // register the function
        FunctionContext &fn = this->global->function_id_to_context.emplace(
                piecewise_construct, forward_as_tuple(function_id),
                forward_as_tuple(this, function_id, method_def.name, method_def.fragments,
                                 method_def.pass_exception_block)).first->second;
        fn.class_id = class_id;

        // link the function as a class attribute
        if (!cls.attribute_indexes.emplace(method_def.name, cls.attributes.size()).second)
        {
            throw logic_error(string_printf("%s.%s overrides a non-method attribute",
                                            def.name, method_def.name));
        }
        cls.attributes.emplace_back(method_def.name, Value(ValueType::Function, function_id));
    }

    // register the class in the module's global namespace
    this->create_global_variable(def.name, Value(ValueType::Class, class_id), false);
    return class_id;
}

bool ModuleContext::is_builtin() const
{
    return !this->ast_root.get();
}


ModuleContext::GlobalVariable::GlobalVariable(const Value &value, size_t index,
                                              int64_t flags) : value(value), index(index), flags(flags)
{}


GlobalContext::GlobalContext(const vector<string> &import_paths) :
        import_paths(import_paths), next_user_function_id(1),
        next_builtin_function_id(-1), next_callsite_token(1)
{
    this->builtins_module = create_builtin_module(this, "builtins");
    if (!this->builtins_module)
    {
        throw logic_error("builtins module does not exist");
    }
}

GlobalContext::~GlobalContext()
{
    for (const auto &it: this->bytes_constants)
    {
        if (debug_flags & DebugFlag::ShowRefcountChanges)
        {
            fprintf(stderr, "[refcount:constants] deleting Bytes constant %s\n",
                    it.second->data);
        }
        delete_reference(it.second);
    }
    for (const auto &it: this->unicode_constants)
    {
        if (debug_flags & DebugFlag::ShowRefcountChanges)
        {
            fprintf(stderr, "[refcount:constants] deleting Unicode constant %ls\n",
                    it.second->data);
        }
        delete_reference(it.second);
    }
}

GlobalContext::UnresolvedFunctionCall::UnresolvedFunctionCall(
        int64_t callee_function_id, const std::vector<Value> &arg_types,
        ModuleContext *caller_module, int64_t caller_function_id,
        int64_t caller_fragment_index, int64_t caller_split_id) :
        callee_function_id(callee_function_id), arg_types(arg_types),
        caller_module(caller_module), caller_function_id(caller_function_id),
        caller_fragment_index(caller_fragment_index),
        caller_split_id(caller_split_id)
{}

string GlobalContext::UnresolvedFunctionCall::str() const
{
    string arg_types_str;
    for (const Value &v: this->arg_types)
    {
        if (!arg_types_str.empty())
        {
            arg_types_str += ',';
        }
        arg_types_str += v.str();
    }
    return string_printf("UnresolvedFunctionCall(%" PRId64 ", [%s], %p(%s), %" PRId64
                         ", %" PRId64 ", %" PRId64 ")", this->callee_function_id, arg_types_str.c_str(),
                         this->caller_module, this->caller_module->name.c_str(), this->caller_function_id,
                         this->caller_fragment_index, this->caller_split_id);
}

static void print_source_location(FILE *stream, shared_ptr<const SourceFile> f,
                                  size_t offset)
{
    size_t line_num = f->line_number_of_offset(offset);
    string line = f->line(line_num);
    fprintf(stream, ">>> %s\n", line.c_str());
    fputs("--- ", stream);
    size_t space_count = offset - f->line_offset(line_num);
    for (; space_count; space_count--)
    {
        fputc(' ', stream);
    }
    fputs("^\n", stream);
}

void GlobalContext::print_compile_error(FILE *stream,
                                        const ModuleContext *module, const compile_error &e)
{
    if (e.where >= 0)
    {
        size_t line_num = module->source->line_number_of_offset(e.where);
        fprintf(stream, "[%s] failure at line %zu (offset %zd): %s\n",
                module->name.c_str(), line_num, e.where, e.what());
        print_source_location(stream, module->source, e.where);
    }
    else
    {
        fprintf(stream, "[%s] failure at indeterminate location: %s\n",
                module->name.c_str(), e.what());
    }
}

shared_ptr<ModuleContext> GlobalContext::get_or_create_module(
        const string &module_name, const string &filename, bool filename_is_code)
{

    // if it already exists, return it
    try
    {
        return this->modules.at(module_name);
    } catch (const out_of_range &e)
    {}

    // if it doesn't exist but is a built-in module, return that
    {
        auto module = create_builtin_module(this, module_name);
        if (module.get())
        {
            this->modules.emplace(module_name, module);
            return module;
        }
    }

    // if code is given, create a module directly from that code
    if (filename_is_code)
    {
        auto module = this->modules.emplace(piecewise_construct,
                                            forward_as_tuple(module_name),
                                            forward_as_tuple(new ModuleContext(this, module_name, filename,
                                                                               true))).first->second;
        if (debug_flags & DebugFlag::ShowSourceDebug)
        {
            fprintf(stderr, "[%s] added code from memory (%zu lines, %zu bytes)\n\n",
                    module_name.c_str(), module->source->line_count(),
                    module->source->file_size());
        }
        return module;
    }

    // if no filename is given, search for the correct file and load it
    string found_filename;
    if (filename.empty())
    {
        found_filename = this->find_source_file(module_name);
    }
    else
    {
        found_filename = filename;
    }
    auto module = this->modules.emplace(piecewise_construct,
                                        forward_as_tuple(module_name),
                                        forward_as_tuple(new ModuleContext(this, module_name, found_filename,
                                                                           false))).first->second;
    if (debug_flags & DebugFlag::ShowSourceDebug)
    {
        fprintf(stderr, "[%s] loaded %s (%zu lines, %zu bytes)\n\n",
                module_name.c_str(), found_filename.c_str(), module->source->line_count(),
                module->source->file_size());
    }
    return module;
}

string GlobalContext::find_source_file(const string &module_name)
{
    string module_path_name = module_name;
    for (char &ch: module_path_name)
    {
        if (ch == '.')
        {
            ch = '/';
        }
    }
    for (const string &path: this->import_paths)
    {
        string filename = path + "/" + module_path_name + ".py";
        try
        {
            stat(filename);
            return filename;
        } catch (const cannot_stat_file &)
        {}
    }

    throw compile_error("can\'t find file for module " + module_name);
}

FunctionContext *GlobalContext::context_for_function(int64_t function_id,
                                                     ModuleContext *module_for_create)
{
    if (function_id == 0)
    {
        return nullptr;
    }
    if (module_for_create)
    {
        return &(*this->function_id_to_context.emplace(piecewise_construct,
                                                       forward_as_tuple(function_id),
                                                       forward_as_tuple(module_for_create, function_id)).first).second;
    }
    else
    {
        try
        {
            return &this->function_id_to_context.at(function_id);
        } catch (const out_of_range &e)
        {
            return nullptr;
        }
    }
}

ClassContext *GlobalContext::context_for_class(int64_t class_id,
                                               ModuleContext *module_for_create)
{
    if (class_id == 0)
    {
        return nullptr;
    }
    if (module_for_create)
    {
        return &(*this->class_id_to_context.emplace(piecewise_construct,
                                                    forward_as_tuple(class_id),
                                                    forward_as_tuple(module_for_create, class_id)).first).second;
    }
    else
    {
        try
        {
            return &this->class_id_to_context.at(class_id);
        } catch (const out_of_range &e)
        {
            return nullptr;
        }
    }
}

int64_t GlobalContext::match_value_to_type(const Value &expected_type,
                                           const Value &value)
{
    if (value.type == ValueType::Indeterminate)
    {
        throw compile_error("matched value is Indeterminate");
    }

    if (expected_type.type == ValueType::Indeterminate)
    {
        return 1;

    }
    else if (expected_type.type != value.type)
    {
        return -1; // no match
    }

    // allow subclasses to match with their parent classes
    if (expected_type.type == ValueType::Instance)
    {
        const auto *cls = this->context_for_class(expected_type.class_id);
        while (cls)
        {
            if (cls->id == value.class_id)
            {
                break;
            }
            cls = this->context_for_class(cls->parent_class_id);
        }

        // cls is non-nullptr if and only if we found a matching (super)class
        return cls ? 0 : -1;
    }

    // if it's not Indeterminate and not a class, check the extension types
    return this->match_values_to_types(
            expected_type.extension_types, value.extension_types);
}

int64_t GlobalContext::match_values_to_types(
        const vector<Value> &expected_types, const vector<Value> &values)
{
    if (expected_types.size() != values.size())
    {
        return -1;
    }

    int64_t promotion_count = 0;
    for (size_t x = 0; x < expected_types.size(); x++)
    {
        int64_t this_value_promotion_count = this->match_value_to_type(
                expected_types[x], values[x]);
        if (this_value_promotion_count < 0)
        {
            return this_value_promotion_count;
        }
        promotion_count += this_value_promotion_count;
    }

    return promotion_count;
}

const BytesObject *GlobalContext::get_or_create_constant(const string &s,
                                                         bool use_shared_constants)
{
    if (!use_shared_constants)
    {
        return bytes_new(s.data(), s.size());
    }

    BytesObject *o = nullptr;
    try
    {
        o = this->bytes_constants.at(s);
    } catch (const out_of_range &e)
    {
        o = bytes_new(s.data(), s.size());
        this->bytes_constants.emplace(s, o);
    }
    return o;
}

const UnicodeObject *GlobalContext::get_or_create_constant(const wstring &s,
                                                           bool use_shared_constants)
{
    if (!use_shared_constants)
    {
        return unicode_new(s.data(), s.size());
    }

    UnicodeObject *o = nullptr;
    try
    {
        o = this->unicode_constants.at(s);
    } catch (const out_of_range &e)
    {
        o = unicode_new(s.data(), s.size());
        this->unicode_constants.emplace(s, o);
    }
    return o;
}

Value GlobalContext::static_attribute_lookup(ModuleContext *module,
                                             const string &name)
{
    if (name.empty())
    {
        throw out_of_range("name is empty");
    }

    vector<string> tokens = split(name, '.');

    for (size_t token_index = 0; token_index < tokens.size() - 1; token_index++)
    {
        auto result = this->static_attribute_lookup(module, tokens[token_index]);
        if (result.type != ValueType::Module)
        {
            throw runtime_error("static attribute lookup passes through non-module");
        }
        auto module_shared = this->get_or_create_module(*result.bytes_value);
        if (module_shared->phase < ModuleContext::Phase::Annotated)
        {
            throw runtime_error("static attribute lookup passes through early-phase module");
        }
        module = module_shared.get();
    }

    const auto &global = module->global_variables.at(tokens.back());
    return global.value;
}

static const unordered_map<string, Value> builtin_type_for_annotation({
                                                                              {"None",  Value(ValueType::None)},
                                                                              {"bool",  Value(ValueType::Bool)},
                                                                              {"int",   Value(ValueType::Int)},
                                                                              {"float", Value(ValueType::Float)},
                                                                              {"bytes", Value(ValueType::Bytes)},
                                                                              {"str",   Value(ValueType::Unicode)},
                                                                              {"list",  Value(ValueType::List)},
                                                                              {"List",  Value(ValueType::List)},
                                                                              {"tuple", Value(ValueType::Tuple)},
                                                                              {"Tuple", Value(ValueType::Tuple)},
                                                                              {"set",   Value(ValueType::Set)},
                                                                              {"Set",   Value(ValueType::Set)},
                                                                              {"dict",  Value(ValueType::Dict)},
                                                                              {"Dict",  Value(ValueType::Dict)},
                                                                      });

Value GlobalContext::type_for_annotation(ModuleContext *module,
                                         shared_ptr<TypeAnnotation> type_annotation)
{
    try
    {
        Value value = this->static_attribute_lookup(module, type_annotation->type_name);
        if (value.type != ValueType::Class)
        {
            throw compile_error("type annotation " + type_annotation->type_name + " does not refer to class type");
        }
        if (!value.value_known)
        {
            throw compile_error("type annotation " + type_annotation->type_name + " refers to unknown-value type");
        }
        return Value(ValueType::Instance, value.class_id, nullptr);

    } catch (const out_of_range &)
    {}

    try
    {
        auto type = builtin_type_for_annotation.at(type_annotation->type_name);
        for (const auto &extension_type_annotation: type_annotation->generic_arguments)
        {
            type.extension_types.emplace_back(this->type_for_annotation(module, extension_type_annotation));
        }
        return type;

    } catch (const out_of_range &)
    {
        throw compile_error("type annotation " + type_annotation->type_name + " refers to unknown type");
    }
}
