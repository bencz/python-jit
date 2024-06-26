#include "AnalysisVisitor.hh"

#include <inttypes.h>
#include <stdio.h>

#include <phosg/Strings.hh>

#include "../Debug.hh"
#include "../AST/PythonLexer.hh"
#include "Compile.hh"

using namespace std;


AnalysisVisitor::AnalysisVisitor(GlobalContext *global, ModuleContext *module)
        : global(global), module(module), in_function_id(0), in_class_id(0),
          last_attribute_lookup_had_class_base(false)
{}

void AnalysisVisitor::visit(UnaryOperation *a)
{
    a->expr->accept(this);
    try
    {
        this->current_value = execute_unary_operator(a->oper, this->current_value);
    } catch (const exception &e)
    {
        throw compile_error(string_printf(
                "unary operator execution failed: %s", e.what()), a->file_offset);
    }
}

void AnalysisVisitor::visit(BinaryOperation *a)
{
    a->left->accept(this);
    Value left = std::move(this->current_value);

    a->right->accept(this);

    try
    {
        this->current_value = execute_binary_operator(a->oper, left,
                                                      this->current_value);
    } catch (const exception &e)
    {
        throw compile_error(string_printf(
                "binary operator execution failed: %s", e.what()), a->file_offset);
    }
}

void AnalysisVisitor::visit(TernaryOperation *a)
{
    a->left->accept(this);
    Value left = std::move(this->current_value);

    a->center->accept(this);
    Value center = std::move(this->current_value);

    a->right->accept(this);

    try
    {
        this->current_value = execute_ternary_operator(a->oper, left, center,
                                                       this->current_value);
    } catch (const exception &e)
    {
        throw compile_error(string_printf(
                "ternary operator execution failed: %s", e.what()), a->file_offset);
    }
}

void AnalysisVisitor::visit(ListConstructor *a)
{
    vector<shared_ptr<Value>> items;
    for (auto item: a->items)
    {
        item->accept(this);
        items.emplace_back(new Value(std::move(this->current_value)));
    }

    a->value_type = compute_list_extension_type(items);
    this->current_value = Value(ValueType::List, std::move(items));
}

void AnalysisVisitor::visit(SetConstructor *a)
{
    unordered_set<Value> items;
    for (auto item: a->items)
    {
        item->accept(this);
        items.emplace(std::move(this->current_value));
    }

    a->value_type = compute_set_extension_type(items);
    this->current_value = Value(ValueType::Set, std::move(items));
}

void AnalysisVisitor::visit(DictConstructor *a)
{
    unordered_map<Value, shared_ptr<Value>> items;
    for (auto item: a->items)
    {
        item.first->accept(this);
        Value key(std::move(this->current_value));
        item.second->accept(this);
        items.emplace(piecewise_construct, forward_as_tuple(std::move(key)),
                      forward_as_tuple(new Value(std::move(this->current_value))));
    }

    auto ex_types = compute_dict_extension_type(items);
    a->key_type = std::move(ex_types.first);
    a->value_type = std::move(ex_types.second);
    this->current_value = Value(ValueType::Dict, std::move(items));
}

void AnalysisVisitor::visit(TupleConstructor *a)
{
    vector<shared_ptr<Value>> items;
    for (auto item: a->items)
    {
        item->accept(this);
        items.emplace_back(new Value(std::move(this->current_value)));
        a->value_types.emplace_back(items.back()->type_only());
    }
    this->current_value = Value(ValueType::Tuple, std::move(items));
}

void AnalysisVisitor::visit(ListComprehension *a)
{
    // for now, just make these unknown-value lists
    this->current_value = Value(ValueType::List);
}

void AnalysisVisitor::visit(SetComprehension *a)
{
    // for now, just make these unknown-value sets
    this->current_value = Value(ValueType::Set);
}

void AnalysisVisitor::visit(DictComprehension *a)
{
    // for now, just make these unknown-value dicts
    this->current_value = Value(ValueType::Dict);
}

void AnalysisVisitor::visit(LambdaDefinition *a)
{
    // TODO: reduce code duplication between here and FunctionDefinition

    // assign all the arguments as Indeterminate for now; we'll come back and
    // fix them later

    int64_t prev_function_id = this->in_function_id;
    this->in_function_id = a->function_id;
    auto *fn = this->current_function();

    for (auto &arg: a->args.args)
    {
        // copy the argument definition into the function context
        fn->args.emplace_back();
        auto &new_arg = fn->args.back();
        new_arg.name = arg.name;
        if (arg.default_value.get())
        {
            arg.default_value->accept(this);
            new_arg.default_value = std::move(this->current_value);
            if (new_arg.default_value.type == ValueType::Indeterminate)
            {
                throw compile_error("default value has Indeterminate type", a->file_offset);
            }
            if (!new_arg.default_value.value_known)
            {
                throw compile_error("can\'t resolve default value", a->file_offset);
            }
        }
        new_arg.type_annotation = arg.type_annotation;
    }
    fn->varargs_name = a->args.varargs_name;
    fn->varkwargs_name = a->args.varkwargs_name;

    a->result->accept(this);
    fn->return_types.emplace(std::move(this->current_value));

    this->in_function_id = prev_function_id;

    this->current_value = Value(ValueType::Function, a->function_id);
}

void AnalysisVisitor::visit(FunctionCall *a)
{
    this->last_attribute_lookup_had_class_base = false;

    // the function reference had better be a function or class
    a->function->accept(this);
    if ((this->current_value.type != ValueType::Function) &&
        (this->current_value.type != ValueType::Class))
    {
        throw compile_error("cannot call a non-function/class object: " + this->current_value.str(), a->file_offset);
    }
    Value function = std::move(this->current_value);

    // if the function a class, then it's actually an __init__ call
    if (function.type == ValueType::Class)
    {
        a->is_class_construction = true;

        // if the function is a function, then it could have been a classmethod call
    }
    else
    {
        if (this->last_attribute_lookup_had_class_base)
        {
            a->is_class_method_call = true;
        }
    }

    // now visit the arg values
    for (auto &arg: a->args)
    {
        arg->accept(this);
    }
    for (auto &it: a->kwargs)
    {
        it.second->accept(this);
    }

    // TODO: typecheck the args if the function's arguments have type annotations

    // we probably can't know the function's return type/value yet, but we'll try
    // to figure it out
    this->current_value = Value(ValueType::Indeterminate);

    // if we know the function's id, annotate the AST node with it
    if (function.value_known)
    {
        a->callee_function_id = function.function_id;

        // if the callee is built-in (has no module) or is in a module in the
        // Analyzed phase or later or is in the current module, then we should know
        // its possible return types
        auto *callee_fn = this->global->context_for_function(a->callee_function_id);
        if (!callee_fn->module || (callee_fn->module == this->module) ||
            (callee_fn->module->phase >= ModuleContext::Phase::Analyzed))
        {
            if (callee_fn->return_types.empty())
            {
                this->current_value = Value(ValueType::None);
            }
            else if (callee_fn->return_types.size() == 1)
            {
                this->current_value = *callee_fn->return_types.begin();
            }
        }
    }
}

void AnalysisVisitor::visit(ArrayIndex *a)
{
    a->array->accept(this);
    if (this->current_value.type == ValueType::Indeterminate)
    {
        // don't even visit the index; we can't know anything about the result type
        return;
    }

    Value array = std::move(this->current_value);

    a->index->accept(this);

    // integer indexes
    if ((array.type == ValueType::Bytes) || (array.type == ValueType::Unicode) ||
        (array.type == ValueType::List) || (array.type == ValueType::Tuple))
    {

        // the index has to be a Bool or Int or Indeterminate (in the last case,
        // value_known will be false)
        if ((this->current_value.type != ValueType::Bool) &&
            (this->current_value.type != ValueType::Int) &&
            (this->current_value.type != ValueType::Indeterminate))
        {
            throw compile_error("array subscript is not Bool or Int", a->file_offset);
        }

        // annotate the AST node if we know the value
        if (this->current_value.value_known)
        {
            a->index_constant = true;
            a->index_value = this->current_value.int_value;
        }

        // if we don't know the array value, we can't know the result type
        if (!array.value_known)
        {
            this->current_value = Value(ValueType::Indeterminate);
            return;
        }
    }

    if (array.type == ValueType::Bytes)
    {
        // if the array is empty, all subscript references throw IndexError

        // if we know the array value but not the index, we can know the result type
        if (!this->current_value.value_known)
        {
            this->current_value = Value(ValueType::Bytes);
            return;
        }

        // get the appropriate item and return it
        int64_t index = this->current_value.int_value;
        if (index < 0)
        {
            index += static_cast<int64_t>(array.bytes_value->size());
        }
        if ((index < 0) || (index >= static_cast<int64_t>(array.bytes_value->size())))
        {
            this->current_value = Value(ValueType::Indeterminate);
        }
        else
        {
            this->current_value = Value(ValueType::Bytes,
                                        array.bytes_value->substr(index, 1));
        }

    }
    else if (array.type == ValueType::Unicode)
    {
        // TODO: deduplicate with the above case somehow

        // if we know the array value but not the index, we can know the result type
        if (!this->current_value.value_known)
        {
            this->current_value = Value(ValueType::Unicode);
            return;
        }

        // get the appropriate item and return it
        int64_t index = this->current_value.int_value;
        if (index < 0)
        {
            index += static_cast<int64_t>(array.unicode_value->size());
        }
        if ((index < 0) || (index >= static_cast<int64_t>(array.unicode_value->size())))
        {
            this->current_value = Value(ValueType::Indeterminate);
        }
        else
        {
            this->current_value = Value(ValueType::Unicode,
                                        array.unicode_value->substr(index, 1));
        }

    }
    else if ((array.type == ValueType::List) || (array.type == ValueType::Tuple))
    {
        // if the array is empty, all subscript references throw IndexError

        // if we know the array value but not the index, we can know the result type
        // if all items in the array have the same type
        if (!this->current_value.value_known)
        {
            // we know list_value isn't empty here
            ValueType return_type = (*array.list_value)[0]->type;
            for (const auto &item: *array.list_value)
            {
                if (return_type != item->type)
                {
                    this->current_value = Value(ValueType::Indeterminate);
                    return;
                }
            }
            this->current_value = Value(return_type);
            return;
        }

        // get the appropriate item and return it
        int64_t index = this->current_value.int_value;
        if (index < 0)
        {
            index += static_cast<int64_t>(array.list_value->size());
        }
        if ((index < 0) || (index >= static_cast<int64_t>(array.list_value->size())))
        {
            this->current_value = Value(ValueType::Indeterminate);
        }
        else
        {
            this->current_value = *(*array.list_value)[index];
        }

        // arbitrary indexes
    }
    else if (array.type == ValueType::Dict)
    {
        // if we don't know the dict value, we can't know the result type
        if (!array.value_known)
        {
            this->current_value = Value(ValueType::Indeterminate);
            return;
        }

        // if we know the dict value but not the index, we can know the result type
        // if all values in the dict have the same type
        if (!this->current_value.value_known)
        {
            auto it = array.dict_value->begin();
            ValueType return_type = it->second->type;
            for (; it != array.dict_value->end(); it++)
            {
                if (return_type != it->second->type)
                {
                    this->current_value = Value(ValueType::Indeterminate);
                    return;
                }
            }
            this->current_value = Value(return_type);
            return;
        }

        // get the appropriate item and return it
        try
        {
            this->current_value = *array.dict_value->at(this->current_value);
        } catch (const out_of_range &e)
        {
            this->current_value = Value(ValueType::Indeterminate);
        }

        // other types don't support subscripts
    }
    else
    {
        string array_str = array.str();
        string index_str = this->current_value.str();
        throw compile_error(string_printf("invalid subscript reference %s[%s]",
                                          array_str.c_str(), index_str.c_str()), a->file_offset);
    }
}

void AnalysisVisitor::visit(ArraySlice *a)
{
    // TODO
    throw compile_error("array slices are currently not supported", a->file_offset);
}

void AnalysisVisitor::visit(IntegerConstant *a)
{
    this->current_value = Value(ValueType::Int, a->value);
}

void AnalysisVisitor::visit(FloatConstant *a)
{
    this->current_value = Value(ValueType::Float, a->value);
}

void AnalysisVisitor::visit(BytesConstant *a)
{
    this->current_value = Value(ValueType::Bytes, a->value);
}

void AnalysisVisitor::visit(UnicodeConstant *a)
{
    this->current_value = Value(ValueType::Unicode, a->value);
}

void AnalysisVisitor::visit(TrueConstant *a)
{
    this->current_value = Value(ValueType::Bool, true);
}

void AnalysisVisitor::visit(FalseConstant *a)
{
    this->current_value = Value(ValueType::Bool, false);
}

void AnalysisVisitor::visit(NoneConstant *a)
{
    this->current_value = Value(ValueType::None);
}

void AnalysisVisitor::visit(VariableLookup *a)
{
    // if the name is built-in, use that instead - we already prevented assignment
    // to built-in names in AnnotationVisitor, so there's no risk of conflict here
    try
    {
        this->current_value = this->global->builtins_module->global_variables.at(a->name).value;
        return;
    } catch (const out_of_range &e)
    {}

    if (this->in_function_id)
    {
        auto *fn = this->current_function();
        try
        {
            this->current_value = fn->locals.at(a->name);
            return;
        } catch (const out_of_range &e)
        {}
    }

    try
    {
        this->current_value = this->module->global_variables.at(a->name).value;
        return;
    } catch (const out_of_range &e)
    {}

    throw compile_error("variable " + a->name + " does not exist", a->file_offset);
}

void AnalysisVisitor::visit(AttributeLookup *a)
{
    a->base->accept(this);

    this->last_attribute_lookup_had_class_base = false;

    int64_t class_id;
    switch (this->current_value.type)
    {
        // this is technically a failure of the compiler
        case ValueType::Indeterminate:
            throw compile_error("attribute lookup on Indeterminate variable", a->file_offset);
        case ValueType::ExtensionTypeReference:
            throw compile_error("attribute lookup on ExtensionTypeReference variable", a->file_offset);

            // these have attributes, but most programs don't use them
        case ValueType::None:
            throw compile_error("attribute lookup on None value", a->file_offset);
        case ValueType::Bool:
            throw compile_error("attribute lookup on Bool value", a->file_offset);
        case ValueType::Int:
            throw compile_error("attribute lookup on Int value", a->file_offset);
        case ValueType::Float:
            throw compile_error("attribute lookup on Float value", a->file_offset);
        case ValueType::Function:
            throw compile_error("attribute lookup on Function value", a->file_offset);

            // look up the class attribute
        case ValueType::Bytes:
            class_id = this->global->BytesObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::Unicode:
            class_id = this->global->UnicodeObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::List:
            class_id = this->global->ListObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::Tuple:
            class_id = this->global->TupleObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::Set:
            class_id = this->global->SetObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::Dict:
            class_id = this->global->DictObject_class_id;
            goto AttributeLookup_resume;
        case ValueType::Class:
            this->last_attribute_lookup_had_class_base = true;
        case ValueType::Instance:
            class_id = this->current_value.class_id;
            goto AttributeLookup_resume;
        AttributeLookup_resume:
        {

            auto *cls = this->global->context_for_class(class_id);
            if (!cls)
            {
                throw compile_error(string_printf(
                                            "attribute lookup refers to missing class: %" PRId64, class_id),
                                    a->file_offset);
            }

            try
            {
                size_t index = cls->attribute_indexes.at(a->name);
                this->current_value = cls->attributes.at(index).value;
            } catch (const out_of_range &e)
            {
                throw compile_error(string_printf(
                                            "class %" PRId64 " attribute lookup refers to missing attribute: %s",
                                            class_id, a->name.c_str()),
                                    a->file_offset);
            }

            // if it isn't a function, it may be mutable - return its type only
            if (this->current_value.type != ValueType::Function)
            {
                this->current_value.clear_value();
            }
            break;
        }

            // we'll need to have the module at Analyzed phase or later
        case ValueType::Module:
        {
            const string &module_name = *this->current_value.bytes_value;
            a->base_module_name = module_name;

            auto module = this->global->get_or_create_module(module_name);
            advance_module_phase(this->global, module.get(), ModuleContext::Phase::Analyzed);
            if (!module.get())
            {
                throw compile_error("attribute lookup refers to missing module",
                                    a->file_offset);
            }

            // just get the value out of the module's globals
            try
            {
                this->current_value = module->global_variables.at(a->name).value;
            } catch (const out_of_range &)
            {
                throw compile_error(string_printf("module %s has no attribute %s",
                                                  module->name.c_str(), a->name.c_str()), a->file_offset);
            }
        }
    }
}

void AnalysisVisitor::visit(TupleLValueReference *a)
{
    // in this visitor, we visit the values before the unpacking tuples, so we
    // can expect this->current_value to be accurate

    if ((this->current_value.type != ValueType::List) && (this->current_value.type != ValueType::Tuple))
    {
        throw compile_error("cannot unpack something that\'s not a List or Tuple", a->file_offset);
    }
    if (!this->current_value.value_known)
    {
        throw compile_error("cannot unpack unknown values", a->file_offset);
    }
    if (this->current_value.list_value->size() != a->items.size())
    {
        throw compile_error("unpacking format length doesn\'t match List/Tuple count", a->file_offset);
    }

    Value base_value = std::move(this->current_value);
    for (size_t x = 0; x < a->items.size(); x++)
    {
        this->current_value = std::move(*(*base_value.list_value)[x]);
        a->items[x]->accept(this);
    }
}

void AnalysisVisitor::visit(ArrayIndexLValueReference *a)
{
    // TODO: for now ignore these
}

void AnalysisVisitor::visit(ArraySliceLValueReference *a)
{
    // TODO: for now ignore these
}

void AnalysisVisitor::visit(AttributeLValueReference *a)
{
    if (!a->base.get() && this->global->builtins_module->global_variables.count(a->name))
    {
        throw compile_error("cannot reassign built-in name " + a->name, a->file_offset);
    }

    // TODO: typecheck the value if a type annotation is present

    // if base is missing, then it's just a simple variable (local/global) write
    if (!a->base.get())
    {
        this->record_assignment(a->name, this->current_value, a->file_offset);

        // if base is present, evaluate it and figure out what it's doing
    }
    else
    {
        Value value = std::move(this->current_value);

        // evaluate the base
        a->base->accept(this);

        if (this->current_value.type == ValueType::Instance)
        {
            // get the class definition and create/overwrite the attribute if possible
            auto *target_cls = this->global->context_for_class(this->current_value.class_id);
            if (!target_cls)
            {
                throw compile_error(string_printf(
                                            "class %" PRId64 " does not have a context", this->current_value.class_id),
                                    a->file_offset);
            }
            this->record_assignment_attribute(target_cls, a->name, value, a->file_offset);

        }
        else if (this->current_value.type == ValueType::Module)
        {
            // the module attribute has to be the same type as the value being
            // written. in order to know this, the module has to be Analyzed
            if (!this->current_value.value_known)
            {
                throw compile_error("base is module, but value is unknown");
            }
            auto target_module = this->global->get_or_create_module(*this->current_value.bytes_value);

            advance_module_phase(this->global, target_module.get(), ModuleContext::Phase::Analyzed);
            if (!target_module)
            {
                throw compile_error(string_printf(
                                            "module %s does not have a context", this->current_value.bytes_value->c_str()),
                                    a->file_offset);
            }
            try
            {
                const auto &var = target_module->global_variables.at(a->name);
                if (!var.value.types_equal(value))
                {
                    string existing_type = var.value.str();
                    string new_type = value.str();
                    throw compile_error(string_printf("%s.%s changes type (from %s to %s)",
                                                      this->current_value.bytes_value->c_str(), a->name.c_str(),
                                                      existing_type.c_str(), new_type.c_str()), a->file_offset);
                }
            } catch (const out_of_range &)
            {
                throw compile_error(string_printf(
                        "module %s does not have attribute %s", this->current_value.bytes_value->c_str(),
                        a->name.c_str()), a->file_offset);
            }

        }
        else
        {
            throw compile_error("cannot write attribute on " + this->current_value.str(),
                                a->file_offset);
        }
    }
}

// statement visitation
void AnalysisVisitor::visit(ModuleStatement *a)
{
    // this is the root call

    for (auto &item: a->items)
    {
        item->accept(this);
    }
}

void AnalysisVisitor::visit(ExpressionStatement *a)
{
    // these are usually function calls or yield statements. in fact, if they
    // don't contain any function calls or yield statements, they cannot have
    // side effects, so we can disregard them entirely. for now we'll just
    // evaluate/analyze them and discard the result.
    // TODO: implement this optimization in the future
    a->expr->accept(this);
}

void AnalysisVisitor::visit(AssignmentStatement *a)
{
    // evaluate expr
    a->value->accept(this);

    // assign to value (the LValueReference visitors will do this)
    a->target->accept(this);
}

void AnalysisVisitor::visit(AugmentStatement *a)
{
    // TODO
    throw compile_error("AugmentStatement not yet implemented", a->file_offset);
}

void AnalysisVisitor::visit(DeleteStatement *a)
{
    auto *fn = this->current_function();
    if (fn)
    {
        // TODO
        throw compile_error("DeleteStatement not yet implemented", a->file_offset);
    }
    else
    {
        // TODO: do we need to support this? seems unlikely
        throw compile_error("DeleteStatement only supported in functions", a->file_offset);
    }
}

void AnalysisVisitor::visit(ImportStatement *a)
{
    // this is similar to AnnotationVisitor, except we copy values too, and we
    // expect all the names to already exist in the target scope

    auto *fn = this->current_function();

    // case 3
    if (a->import_star)
    {
        throw compile_error("import * is not supported", a->file_offset);
    }

    // case 1: import entire modules, not specific names
    if (a->names.empty())
    {
        // we actually don't need to do anything here - AnnotationVisitor already
        // created the correct value type and linked it to the module object
        return;
    }

    // case 2: import some names from a module
    const string &module_name = a->modules.begin()->first;
    auto module = this->global->get_or_create_module(module_name);
    advance_module_phase(this->global, module.get(), ModuleContext::Phase::Analyzed);
    for (const auto &it: a->names)
    {
        if (fn)
        {
            fn->locals.at(it.second) = module->global_variables.at(it.first).value;
        }
        else
        {
            auto &target_var = this->module->global_variables.at(it.second);
            target_var.value = module->global_variables.at(it.first).value;
        }
    }
}

void AnalysisVisitor::visit(GlobalStatement *a)
{
    // nothing to do here; AnnotationVisitor already extracted all useful info
}

void AnalysisVisitor::visit(ExecStatement *a)
{
    // we don't support this
    throw compile_error("ExecStatement is not supported", a->file_offset);
}

void AnalysisVisitor::visit(AssertStatement *a)
{
    // run the check
    a->check->accept(this);

    // if we don't know what the check returned, assume we have to evaluate the
    // message
    if ((!this->current_value.value_known || !this->current_value.truth_value()) &&
        a->failure_message.get())
    {
        a->failure_message->accept(this);

        // the message must be a unicode object
        if (this->current_value.type != ValueType::Unicode)
        {
            throw compile_error("assertion failure message is not Unicode", a->file_offset);
        }
    }
}

void AnalysisVisitor::visit(BreakStatement *a)
{
    // this is static analysis, not execution; we don't do anything here
}

void AnalysisVisitor::visit(ContinueStatement *a)
{
    // this is static analysis, not execution; we don't do anything here
}

void AnalysisVisitor::visit(ReturnStatement *a)
{
    // this tells us what the return type of the function is
    auto *fn = this->current_function();
    if (!fn)
    {
        throw compile_error("return statement outside function", a->file_offset);
    }

    // TODO: typecheck the value if the function has a return type annotation

    if (a->value.get())
    {
        if (fn->is_class_init())
        {
            throw compile_error("__init__ cannot return a value");
        }

        a->value->accept(this);
        fn->return_types.emplace(std::move(this->current_value));
    }
    else
    {
        fn->return_types.emplace(ValueType::None);
    }
}

void AnalysisVisitor::visit(YieldStatement *a)
{
    a->expr->accept(this);
}

void AnalysisVisitor::visit(SingleIfStatement *a)
{
    throw logic_error("SingleIfStatement used instead of subclass");
}

void AnalysisVisitor::visit(IfStatement *a)
{
    a->check->accept(this);
    Value check_result = std::move(this->current_value);

    // if we know the value and it's truthy, skip all the elif/else branches
    if (check_result.value_known && check_result.truth_value())
    {
        a->always_true = true;
        this->visit_list(a->items);
        return;
    }

    // if we know the value and it's falsey, then skip this branch and advance
    // to the elifs
    // TODO: there may be more optimizations we can do here (e.g. if one of the
    // elifs is known and truthy, skip the rest and the else suite)
    if (check_result.value_known && !check_result.truth_value())
    {
        a->always_false = true;

        for (auto &elif: a->elifs)
        {
            elif->accept(this);
        }
        if (a->else_suite.get())
        {
            a->else_suite->accept(this);
        }
        return;
    }

    // we don't know the truth value of the condition; visit all branches
    this->visit_list(a->items);
    for (auto &elif: a->elifs)
    {
        elif->accept(this);
    }
    if (a->else_suite.get())
    {
        a->else_suite->accept(this);
    }
}

void AnalysisVisitor::visit(ElseStatement *a)
{
    this->visit_list(a->items);
}

void AnalysisVisitor::visit(ElifStatement *a)
{
    a->check->accept(this);

    if (this->current_value.value_known)
    {
        if (this->current_value.truth_value())
        {
            a->always_true = true;
        }
        else
        {
            a->always_false = true;
        }
    }

    // if we don't know the value or it's truthy, skip this branch
    if (!this->current_value.value_known || a->always_true)
    {
        this->visit_list(a->items);
    }
}

void AnalysisVisitor::visit(ForStatement *a)
{
    a->collection->accept(this);

    // if the current value is known, we can at least get the types of the values
    if (this->current_value.value_known)
    {
        switch (this->current_value.type)
        {
            // if we don't know the collection type, we can't know the value type;
            // just proceed without knowing
            case ValueType::Indeterminate:
                throw compile_error("encountered known value of Indeterminate type");
            case ValueType::ExtensionTypeReference:
                throw compile_error("encountered known value of ExtensionTypeReference type");

                // silly programmer; you can't iterate these types
            case ValueType::None:
            case ValueType::Bool:
            case ValueType::Int:
            case ValueType::Float:
            case ValueType::Function:
            case ValueType::Class:
            case ValueType::Instance: // TODO: these may be iterable in the future
            case ValueType::Module:
            {
                string target_value = this->current_value.str();
                throw compile_error(string_printf(
                                            "iteration target %s is not a collection", target_value.c_str()),
                                    a->file_offset);
            }

                // these you can iterate. if all the values are the same type, then we can
                // know what the result type is; otherwise, it's Indeterminate
            case ValueType::Bytes:
            case ValueType::Unicode:
                this->current_value = Value(this->current_value.type);
                break;

            case ValueType::List:
            case ValueType::Tuple:
            {
                ValueType extension_type = ValueType::Indeterminate;
                for (const auto &item: *this->current_value.list_value)
                {
                    // if we encounter a single Indeterminate item, the entire result is
                    // Indeterminate
                    if (item->type == ValueType::Indeterminate)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;

                        // if we don't yet know the type, then record it
                    }
                    else if (extension_type == ValueType::Indeterminate)
                    {
                        extension_type = item->type;

                        // if this item's type doesn't match the type we've found so far, the
                        // entire result is Indeterminate
                    }
                    else if (extension_type != item->type)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;
                    }
                }
                this->current_value = Value(extension_type);
                break;
            }

            case ValueType::Set:
            {
                // same logic as for List/Tuple
                // TODO: deduplicate this
                ValueType extension_type = ValueType::Indeterminate;
                for (const auto &item: *this->current_value.set_value)
                {
                    if (item.type == ValueType::Indeterminate)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;
                    }
                    else if (extension_type == ValueType::Indeterminate)
                    {
                        extension_type = item.type;
                    }
                    else if (extension_type != item.type)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;
                    }
                }
                this->current_value = Value(extension_type);
                break;
            }

            case ValueType::Dict:
            {
                // same logic as for List/Tuple, except just use the keys
                // TODO: deduplicate this
                ValueType extension_type = ValueType::Indeterminate;
                for (const auto &item: *this->current_value.dict_value)
                {
                    if (item.first.type == ValueType::Indeterminate)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;
                    }
                    else if (extension_type == ValueType::Indeterminate)
                    {
                        extension_type = item.first.type;
                    }
                    else if (extension_type != item.first.type)
                    {
                        extension_type = ValueType::Indeterminate;
                        break;
                    }
                }
                this->current_value = Value(extension_type);
                break;
            }
        }

    }
    else
    { // value not known
        switch (this->current_value.type)
        {
            case ValueType::ExtensionTypeReference:
                throw compile_error("encountered collection of ExtensionTypeReference type");

                // if we don't know the collection type, we can't know the value type;
                // just proceed without knowing
            case ValueType::Indeterminate:

                // for these we can't know what the result type will be without also
                // knowing the value
            case ValueType::List:
            case ValueType::Tuple:
            case ValueType::Set:
            case ValueType::Dict:
                this->current_value = Value(ValueType::Indeterminate);
                break;

                // silly programmer; you can't iterate these types
            case ValueType::None:
            case ValueType::Bool:
            case ValueType::Int:
            case ValueType::Float:
            case ValueType::Function:
            case ValueType::Class:
            case ValueType::Instance: // these may be iterable in the future
            case ValueType::Module:
            {
                string target_type = this->current_value.str();
                throw compile_error(string_printf(
                                            "iteration target of type %s is not a collection", target_type.c_str()),
                                    a->file_offset);
            }

                // even if we don't know the value, we know what type the result will be
            case ValueType::Bytes:
            case ValueType::Unicode:
                this->current_value = Value(this->current_value.type);
                break;
        }
    }

    a->variable->accept(this);

    this->visit_list(a->items);
    if (a->else_suite.get())
    {
        a->else_suite->accept(this);
    }
}

void AnalysisVisitor::visit(WhileStatement *a)
{
    a->condition->accept(this);
    this->visit_list(a->items);
    if (a->else_suite.get())
    {
        a->else_suite->accept(this);
    }
}

void AnalysisVisitor::visit(ExceptStatement *a)
{
    if (a->types.get())
    {
        a->types->accept(this);
    }

    // parse the types value
    // TODO: currently we only support exception handling where the types are
    // statically resolvable, and the types must be either a single class or a
    // tuple of classes
    if (this->current_value.type == ValueType::Class)
    {
        a->class_ids.emplace(this->current_value.class_id);

    }
    else if (this->current_value.type == ValueType::Tuple)
    {
        for (const auto &type: *this->current_value.list_value)
        {
            if (type->type != ValueType::Class)
            {
                throw compile_error("invalid exception type: " + type->str(),
                                    a->file_offset);
            }
            a->class_ids.emplace(type->class_id);
        }

    }
    else
    {
        throw compile_error("invalid exception type: " + this->current_value.str(),
                            a->file_offset);
    }

    // TODO: support catching multiple exception types in one statement
    if (a->class_ids.size() != 1)
    {
        throw compile_error("except statement does not catch exactly one type",
                            a->file_offset);
    }

    if (!a->name.empty())
    {
        this->record_assignment(a->name, Value(ValueType::Instance,
                                               *a->class_ids.begin(), nullptr), a->file_offset);
    }

    this->visit_list(a->items);
}

void AnalysisVisitor::visit(FinallyStatement *a)
{
    this->visit_list(a->items);
}

void AnalysisVisitor::visit(TryStatement *a)
{
    this->visit_list(a->items);

    for (auto &except: a->excepts)
    {
        except->accept(this); // lolz
    }
    if (a->else_suite.get())
    {
        a->else_suite->accept(this);
    }
    if (a->finally_suite.get())
    {
        a->finally_suite->accept(this);
    }
}

void AnalysisVisitor::visit(WithStatement *a)
{
    for (auto &it: a->item_to_name)
    {
        it.first->accept(this);
        if (!it.second.empty())
        {
            this->record_assignment(it.second, this->current_value, a->file_offset);
        }
    }
    this->visit_list(a->items);
}

void AnalysisVisitor::visit(FunctionDefinition *a)
{
    // TODO: reduce code duplication between here and LambdaDefinition

    auto *fn = this->global->context_for_function(a->function_id);

    // if this is a class method, check if it's an override
    if (this->in_class_id && !this->in_function_id)
    {
        auto *cls = this->current_class();
        try
        {
            size_t index = cls->attribute_indexes.at(a->name);
            auto &attr = cls->attributes.at(index);

            // if it's Indeterminate, then it's not an override
            if (attr.value.type == ValueType::Indeterminate)
            {
                throw out_of_range("not an override");
            }

            if (attr.value.type != ValueType::Function)
            {
                throw compile_error(string_printf("non-function attribute %s cannot be overridden by a function",
                                                  attr.name.c_str()), a->file_offset);
            }

            // functions must be statically resolvable when overriding
            if (!attr.value.value_known)
            {
                throw compile_error("can\'t resolve overridden function id", a->file_offset);
            }

            // if it's not __init__, argument types must match the previous value
            // (inherited from the parent)
            attr.value.function_id = a->function_id;

        } catch (const out_of_range &)
        {
            // not an override
            this->record_assignment(a->name,
                                    Value(ValueType::Function, a->function_id), a->file_offset);
        }

    }
    else
    {
        // record the assignment of the function object to the function's name first
        // in order to handle recursion properly
        this->record_assignment(a->name,
                                Value(ValueType::Function, a->function_id), a->file_offset);
    }

    if (!a->decorators.empty())
    {
        throw compile_error("decorators not yet supported", a->file_offset);
    }

    int64_t prev_function_id = this->in_function_id;
    this->in_function_id = a->function_id;

    // assign types to the arguments based on the type annotations. if there are
    // none, then assign the arguments as Indeterminate for now; we'll come back
    // and fix them later
    for (size_t x = 0; x < a->args.args.size(); x++)
    {
        auto &arg = a->args.args[x];

        // copy the argument definition into the function context
        fn->args.emplace_back();
        auto &new_arg = fn->args.back();
        new_arg.name = arg.name;
        new_arg.type_annotation = arg.type_annotation;

        // if in a class definition, the first argument cannot have a default value
        // or type annotation and must be named "self"
        // TODO: when we support warnings, this should be a warning, not an error
        if (!x && this->in_class_id)
        {
            if (arg.default_value.get())
            {
                throw compile_error(
                        "first argument to instance method cannot have a default value",
                        a->file_offset);
            }
            if (arg.type_annotation.get())
            {
                throw compile_error(
                        "first argument to instance method cannot have a type annotation",
                        a->file_offset);
            }
            if (arg.name != "self")
            {
                throw compile_error(
                        "first argument to instance method must be named \'self\'",
                        a->file_offset);
            }

            // the first argument is the class object - we know its type but not its
            // value
            fn->locals.at(arg.name) = Value(ValueType::Instance, this->in_class_id,
                                            nullptr);

            // if the arg has a default value, infer the type from that
        }
        else if (arg.default_value.get())
        {
            arg.default_value->accept(this);
            new_arg.default_value = std::move(this->current_value);
            if (new_arg.default_value.type == ValueType::Indeterminate)
            {
                throw compile_error("default value has Indeterminate type", a->file_offset);
            }
            if (!new_arg.default_value.value_known)
            {
                throw compile_error("can\'t resolve default value", a->file_offset);
            }

            // if there's a type annotation, it should agree with the default value
            if (arg.type_annotation.get())
            {
                Value type_from_annotation = this->global->type_for_annotation(
                        this->module, arg.type_annotation);
                if (!type_from_annotation.types_equal(new_arg.default_value))
                {
                    throw compile_error("default value does not match type annotation", a->file_offset);
                }
            }

            fn->locals.at(arg.name) = new_arg.default_value.type_only();

            // if the arg has a type annotation, use that as the type
        }
        else if (arg.type_annotation.get())
        {
            fn->locals.at(arg.name) = this->global->type_for_annotation(this->module,
                                                                        arg.type_annotation);
        }

        // if none of the above, then we can't infer the type from anything; just
        // leave it as Indeterminate for now
    }

    fn->varargs_name = a->args.varargs_name;
    fn->varkwargs_name = a->args.varkwargs_name;

    this->visit_list(a->items);

    // if this is an __init__ function, it returns a class object
    if (fn->is_class_init())
    {
        if (!fn->return_types.empty())
        {
            throw compile_error("__init__ cannot return a value");
        }

        fn->return_types.emplace(ValueType::Instance, fn->id, nullptr);

    }
    else
    {
        // check if the return types match the type annotation
        if (a->return_type_annotation.get())
        {
            fn->annotated_return_type = this->global->type_for_annotation(
                    this->module, a->return_type_annotation);
            for (const Value &return_type: fn->return_types)
            {
                // allow Indeterminate. this just means we don't know the type yet;
                // we'll check again when compiling the return statement
                if (return_type.type == ValueType::Indeterminate)
                {
                    continue;
                }
                if (this->global->match_value_to_type(fn->annotated_return_type, return_type) < 0)
                {
                    throw compile_error("function return type does not match type annotation", a->file_offset);
                }
            }
        }

        // if there's only one return type and it's None, delete it
        if ((fn->return_types.size() == 1) && (fn->return_types.begin()->type == ValueType::None))
        {
            fn->return_types.clear();
        }
    }

    // TODO: check that __init__ either calls super.__init__ or initializes all
    // attributes

    this->in_function_id = prev_function_id;
}

void AnalysisVisitor::visit(ClassDefinition *a)
{
    if (!a->decorators.empty())
    {
        throw compile_error("decorators not yet supported", a->file_offset);
    }
    if (a->parent_types.size() > 1)
    {
        throw compile_error("multiple inheritance is not yet supported", a->file_offset);
    }

    // if there's a parent class, bring in all of its attributes
    if (!a->parent_types.empty())
    {
        auto *cls = this->global->context_for_class(a->class_id);
        if (!cls)
        {
            throw compile_error("class does not exist at analysis time");
        }

        a->parent_types[0]->accept(this);
        if (this->current_value.type != ValueType::Class)
        {
            throw compile_error("base class is " + this->current_value.str() + " which is not a class type");
        }

        int64_t parent_class_id = this->current_value.class_id;
        auto *parent_cls = this->global->context_for_class(parent_class_id);
        if (!cls)
        {
            throw compile_error("parent class does not exist at analysis time");
        }

        // this class' attributes were created during annotation, so we probably
        // have to shift them down a bit to make room for the parent class'
        // attributes, which should come first
        vector<ClassContext::ClassAttribute> new_attributes = parent_cls->attributes;
        unordered_map<string, size_t> new_attribute_indexes = parent_cls->attribute_indexes;

        // add new attrs on the end
        for (const auto &attr: cls->attributes)
        {
            auto emplace_ret = new_attribute_indexes.emplace(attr.name, new_attributes.size());
            if (emplace_ret.second)
            {
                new_attributes.emplace_back(attr);
            }
        }

        cls->attributes = new_attributes;
        cls->attribute_indexes = new_attribute_indexes;
    }

    int64_t prev_class_id = this->in_class_id;
    this->in_class_id = a->class_id;

    this->visit_list(a->items);

    // TODO: set these attributes on the class when it's finalized:
    // __base__
    // __bases__
    // __basicsize__
    // __class__
    // __doc__
    // __flags__
    // __itemsize__
    // __module__
    // __name__
    // __qualname__
    // __sizeof__
    // __weakref__

    this->in_class_id = prev_class_id;

    this->record_assignment(a->name, Value(ValueType::Class, a->class_id),
                            a->file_offset);
}

FunctionContext *AnalysisVisitor::current_function()
{
    return this->global->context_for_function(this->in_function_id);
}

ClassContext *AnalysisVisitor::current_class()
{
    return this->global->context_for_class(this->in_class_id);
}


void AnalysisVisitor::record_assignment_generic(Value &var,
                                                const string &name, const Value &value, size_t file_offset)
{
    if (var.type == ValueType::Indeterminate)
    {
        var = value; // this is the first write
    }
    else
    {
        if (!var.types_equal(value))
        {
            string existing_type = var.str();
            string new_type = value.str();
            throw compile_error(string_printf("%s changes type (from %s to %s)",
                                              name.c_str(), existing_type.c_str(), new_type.c_str()), file_offset);
        }

        if (var != value)
        {
            var.clear_value();
        }
    }
}

void AnalysisVisitor::record_assignment_global(const string &name,
                                               const Value &value, size_t file_offset)
{
    this->record_assignment_generic(this->module->global_variables.at(name).value,
                                    name, value, file_offset);
}

void AnalysisVisitor::record_assignment_local(FunctionContext *fn,
                                              const string &name, const Value &value, size_t file_offset)
{
    try
    {
        this->record_assignment_generic(fn->locals.at(name), name, value, file_offset);
    } catch (const out_of_range &e)
    {
        throw compile_error(string_printf(
                "local variable %s not found in annotation phase",
                name.c_str()), file_offset);
    }
}

void AnalysisVisitor::record_assignment_attribute(ClassContext *cls,
                                                  const string &name, const Value &value, size_t file_offset)
{
    try
    {
        size_t index = cls->attribute_indexes.at(name);
        this->record_assignment_generic(cls->attributes.at(index).value, name,
                                        value, file_offset);

    } catch (const out_of_range &e)
    {
        throw compile_error("class does not have attribute " + name + "; it must be assigned in __init__",
                            file_offset);
    }
}

void AnalysisVisitor::record_assignment(const string &name, const Value &var,
                                        size_t file_offset)
{

    auto *fn = this->current_function();
    if (fn)
    {
        if (fn->explicit_globals.count(name))
        {
            this->record_assignment_global(name, var, file_offset);
        }
        else
        {
            this->record_assignment_local(fn, name, var, file_offset);
        }
        return;
    }

    auto *cls = this->current_class();
    if (cls)
    {
        this->record_assignment_attribute(cls, name, var, file_offset);
        return;
    }

    this->record_assignment_global(name, var, file_offset);
}
