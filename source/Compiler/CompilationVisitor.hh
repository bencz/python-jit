#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <libamd64/AMD64Assembler.hh>

#include "../AST/SourceFile.hh"
#include "../AST/PythonASTNodes.hh"
#include "../AST/PythonASTVisitor.hh"
#include "../Environment/Value.hh"
#include "Contexts.hh"


class CompilationVisitor : public RecursiveASTVisitor
{
public:
    class terminated_by_split : public std::runtime_error
    {
    public:
        explicit terminated_by_split(int64_t callsite_token);

        ~terminated_by_split() override = default;

        int64_t callsite_token;
    };

    CompilationVisitor(GlobalContext *global, ModuleContext *module,
                       Fragment *fragment);

    ~CompilationVisitor() override = default;

    AMD64Assembler &assembler();

    const std::unordered_set<Value> &return_types() const;

    size_t get_file_offset() const;

    using RecursiveASTVisitor::visit;

    // expression evaluation
    void visit(UnaryOperation *a) override;
    void visit(BinaryOperation *a) override;
    void visit(TernaryOperation *a) override;
    void visit(ListConstructor *a) override;
    void visit(SetConstructor *a) override;
    void visit(DictConstructor *a) override;
    void visit(TupleConstructor *a) override;
    void visit(ListComprehension *a) override;
    void visit(SetComprehension *a) override;
    void visit(DictComprehension *a) override;
    void visit(LambdaDefinition *a) override;
    void visit(FunctionCall *a) override;
    void visit(ArrayIndex *a) override;
    void visit(ArraySlice *a) override;
    void visit(IntegerConstant *a) override;
    void visit(FloatConstant *a) override;
    void visit(BytesConstant *a) override;
    void visit(UnicodeConstant *a) override;
    void visit(TrueConstant *a) override;
    void visit(FalseConstant *a) override;
    void visit(NoneConstant *a) override;
    void visit(VariableLookup *a) override;
    void visit(AttributeLookup *a) override;
    void visit(AttributeLValueReference *a) override;
    void visit(ArrayIndexLValueReference *a) override;
    void visit(ArraySliceLValueReference *a) override;
    void visit(TupleLValueReference *a) override;
    void visit(ModuleStatement *a) override;
    void visit(ExpressionStatement *a) override;
    void visit(AssignmentStatement *a) override;
    void visit(AugmentStatement *a) override;
    void visit(DeleteStatement *a) override;
    void visit(ImportStatement *a) override;
    void visit(GlobalStatement *a) override;
    void visit(ExecStatement *a) override;
    void visit(AssertStatement *a) override;
    void visit(BreakStatement *a) override;
    void visit(ContinueStatement *a) override;
    void visit(ReturnStatement *a) override;
    void visit(RaiseStatement *a) override;
    void visit(YieldStatement *a) override;
    void visit(SingleIfStatement *a) override;
    void visit(IfStatement *a) override;
    void visit(ElseStatement *a) override;
    void visit(ElifStatement *a) override;
    void visit(ForStatement *a) override;
    void visit(WhileStatement *a) override;
    void visit(ExceptStatement *a) override;
    void visit(FinallyStatement *a) override;
    void visit(TryStatement *a) override;
    void visit(WithStatement *a) override;
    void visit(FunctionDefinition *a) override;
    void visit(ClassDefinition *a) override;

private:
    // debugging info
    ssize_t file_offset;

    // environment
    GlobalContext *global;
    ModuleContext *module;
    Fragment *fragment;

    // output values
    std::unordered_set<Value> function_return_types;

    // compilation state
    union
    {
        int64_t available_registers{}; // bit mask; check for (1 << register)
        struct
        {
            int32_t available_int_registers;
            int32_t available_float_registers;
        };
    };
    Register target_register;
    Register float_target_register;
    int64_t stack_bytes_used;
    std::unordered_map<std::string, int64_t> variable_to_stack_offset;
    std::unordered_map<std::string, Value> local_variable_types;

    std::string return_label;
    std::string exception_return_label;
    std::vector<std::string> break_label_stack;
    std::vector<std::string> continue_label_stack;

    struct VariableLocation
    {
        std::string name;
        Value type;

        ModuleContext *global_module; // can be nullptr for locals/attributes
        int64_t global_index;

        MemoryReference variable_mem;
        bool variable_mem_valid;

        VariableLocation();

        std::string str() const;
    };

    Value current_type;
    bool holding_reference;

    bool evaluating_instance_pointer;
    bool in_finally_block;

    // output manager
    AMD64Assembler as;

    Register reserve_register(Register which = Register::None,
                              bool float_register = false);

    void release_register(Register reg, bool float_register = false);

    void release_all_registers(bool float_registers = false);

    bool register_is_available(Register which, bool float_register = false);

    Register available_register(Register preferred = Register::None,
                                bool float_register = false);

    Register available_register_except(
            const std::vector<Register> &prevented_registers,
            bool float_register = false);

    int64_t write_push_reserved_registers();

    void write_pop_reserved_registers(int64_t registers);

    bool is_always_truthy(const Value &type);

    bool is_always_falsey(const Value &type);

    void write_current_truth_value_test();

    void write_code_for_value(const Value &value);

    void assert_not_evaluating_instance_pointer();

    ssize_t write_function_call_stack_prep(size_t arg_count = 0);

    void write_function_call(const MemoryReference &function_loc,
                             const std::vector<MemoryReference> &args,
                             const std::vector<MemoryReference> &float_args,
                             ssize_t arg_stack_bytes = -1, Register return_register = Register::None,
                             bool return_float = false);

    void write_function_setup(const std::string &base_label, bool setup_special_regs);

    void write_function_cleanup(const std::string &base_label, bool setup_special_regs);

    void write_add_reference(Register addr_reg);

    void write_delete_held_reference(const MemoryReference &mem);

    void write_delete_reference(const MemoryReference &mem, ValueType type);

    void write_alloc_class_instance(int64_t class_id, bool initialize_attributes = true);

    void write_raise_exception(int64_t class_id, const wchar_t *message = nullptr);

    void write_create_exception_block(
            const std::vector<std::pair<std::string, std::unordered_set<int64_t>>> &label_to_class_ids,
            const std::string &exception_return_label);

    void write_push(Register reg);

    void write_push(const MemoryReference &mem);

    void write_push(int64_t value);

    void write_pop(Register reg);

    void adjust_stack(ssize_t bytes, bool write_opcode = true);

    void adjust_stack_to(ssize_t bytes, bool write_opcode = true);

    void write_load_double(Register reg, double value);

    void write_read_variable(Register target_register,
                             Register float_target_register, const VariableLocation &loc);

    void write_write_variable(Register value_register,
                              Register float_value_register, const VariableLocation &loc);

    VariableLocation location_for_global(ModuleContext *module,
                                         const std::string &name);

    VariableLocation location_for_variable(const std::string &name);

    VariableLocation location_for_attribute(ClassContext *cls,
                                            const std::string &name, Register instance_reg);

    FunctionContext *current_function();
};
