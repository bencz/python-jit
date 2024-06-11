#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../Environment/Value.hh"
#include "../AST/SourceFile.hh"
#include "../AST/PythonASTNodes.hh"
#include "../AST/PythonASTVisitor.hh"
#include "Contexts.hh"


class AnnotationVisitor : public RecursiveASTVisitor
{
public:
    AnnotationVisitor(GlobalContext *global, ModuleContext *module);
    ~AnnotationVisitor() override = default;

    using RecursiveASTVisitor::visit;

    void visit(ImportStatement *a) override;
    void visit(GlobalStatement *a) override;
    void visit(VariableLookup *a) override;
    void visit(AttributeLValueReference *a) override;
    void visit(ExceptStatement *a) override;
    void visit(FunctionDefinition *a) override;
    void visit(LambdaDefinition *a) override;
    void visit(ClassDefinition *a) override;
    void visit(UnaryOperation *a) override;
    void visit(YieldStatement *a) override;
    void visit(FunctionCall *a) override;
    void visit(ModuleStatement *a) override;

private:
    GlobalContext *global;
    ModuleContext *module;

    // temporary state
    int64_t in_function_id;
    int64_t in_class_id;
    bool in_class_init{};
    const VariableLookup *last_variable_lookup_node{};

    FunctionContext *current_function();
    ClassContext *current_class();
    void record_write(const std::string &name, size_t file_offset);
    void record_class_attribute_write(const std::string &name, size_t file_offset);
};
