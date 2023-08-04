// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-graph-builder.h"

#include <algorithm>
#include <limits>

#include "src/base/logging.h"
#include "src/base/optional.h"
#include "src/base/v8-fallthrough.h"
#include "src/base/vector.h"
#include "src/builtins/builtins-constructor.h"
#include "src/builtins/builtins.h"
#include "src/codegen/cpu-features.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/compiler/access-info.h"
#include "src/compiler/bytecode-liveness-map.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/feedback-source.h"
#include "src/compiler/heap-refs.h"
#include "src/compiler/js-heap-broker-inl.h"
#include "src/compiler/processed-feedback.h"
#include "src/deoptimizer/deoptimize-reason.h"
#include "src/flags/flags.h"
#include "src/handles/maybe-handles-inl.h"
#include "src/ic/handler-configuration-inl.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/interpreter/bytecode-flags.h"
#include "src/interpreter/bytecode-register.h"
#include "src/interpreter/bytecodes.h"
#include "src/maglev/maglev-compilation-info.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-graph-printer.h"
#include "src/maglev/maglev-interpreter-frame-state.h"
#include "src/maglev/maglev-ir.h"
#include "src/objects/elements-kind.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/fixed-array.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/literal-objects-inl.h"
#include "src/objects/name-inl.h"
#include "src/objects/property-cell.h"
#include "src/objects/property-details.h"
#include "src/objects/shared-function-info.h"
#include "src/objects/slots-inl.h"
#include "src/objects/type-hints.h"
#include "src/roots/roots.h"
#include "src/utils/utils.h"

namespace v8::internal::maglev {

namespace {

enum class CpuOperation {
  kFloat64Round,
};

// TODO(leszeks): Add a generic mechanism for marking nodes as optionally
// supported.
bool IsSupported(CpuOperation op) {
  switch (op) {
    case CpuOperation::kFloat64Round:
#if defined(V8_TARGET_ARCH_X64)
      return CpuFeatures::IsSupported(SSE4_1) || CpuFeatures::IsSupported(AVX);
#elif defined(V8_TARGET_ARCH_ARM)
      return CpuFeatures::IsSupported(ARMv8);
#elif defined(V8_TARGET_ARCH_ARM64)
      return true;
#else
#error "Maglev does not support this architecture."
#endif
  }
}

ValueNode* TryGetParentContext(ValueNode* node) {
  if (CreateFunctionContext* n = node->TryCast<CreateFunctionContext>()) {
    return n->context().node();
  }

  if (CallRuntime* n = node->TryCast<CallRuntime>()) {
    switch (n->function_id()) {
      case Runtime::kPushBlockContext:
      case Runtime::kPushCatchContext:
      case Runtime::kNewFunctionContext:
        return n->context().node();
      default:
        break;
    }
  }

  return nullptr;
}

// Attempts to walk up the context chain through the graph in order to reduce
// depth and thus the number of runtime loads.
void MinimizeContextChainDepth(ValueNode** context, size_t* depth) {
  while (*depth > 0) {
    ValueNode* parent_context = TryGetParentContext(*context);
    if (parent_context == nullptr) return;
    *context = parent_context;
    (*depth)--;
  }
}

class FunctionContextSpecialization final : public AllStatic {
 public:
  static compiler::OptionalContextRef TryToRef(
      const MaglevCompilationUnit* unit, ValueNode* context, size_t* depth) {
    DCHECK(unit->info()->specialize_to_function_context());
    if (Constant* n = context->TryCast<Constant>()) {
      return n->ref().AsContext().previous(unit->broker(), depth);
    }
    return {};
  }
};

}  // namespace

class CallArguments {
 public:
  enum Mode {
    kDefault,
    kWithSpread,
    kWithArrayLike,
  };

  CallArguments(ConvertReceiverMode receiver_mode,
                interpreter::RegisterList reglist,
                const InterpreterFrameState& frame, Mode mode = kDefault)
      : receiver_mode_(receiver_mode),
        args_(reglist.register_count()),
        mode_(mode) {
    for (int i = 0; i < reglist.register_count(); i++) {
      args_[i] = frame.get(reglist[i]);
    }
    DCHECK_IMPLIES(args_.size() == 0,
                   receiver_mode == ConvertReceiverMode::kNullOrUndefined);
    DCHECK_IMPLIES(mode != kDefault,
                   receiver_mode == ConvertReceiverMode::kAny);
    DCHECK_IMPLIES(mode == kWithArrayLike, args_.size() == 2);
  }

  explicit CallArguments(ConvertReceiverMode receiver_mode)
      : receiver_mode_(receiver_mode), args_(), mode_(kDefault) {
    DCHECK_EQ(receiver_mode, ConvertReceiverMode::kNullOrUndefined);
  }

  CallArguments(ConvertReceiverMode receiver_mode,
                std::initializer_list<ValueNode*> args, Mode mode = kDefault)
      : receiver_mode_(receiver_mode), args_(args), mode_(mode) {
    DCHECK_IMPLIES(mode != kDefault,
                   receiver_mode == ConvertReceiverMode::kAny);
    DCHECK_IMPLIES(mode == kWithArrayLike, args_.size() == 2);
  }

  ValueNode* receiver() const {
    if (receiver_mode_ == ConvertReceiverMode::kNullOrUndefined) {
      return nullptr;
    }
    return args_[0];
  }

  void set_receiver(ValueNode* receiver) {
    if (receiver_mode_ == ConvertReceiverMode::kNullOrUndefined) {
      args_.insert(args_.data(), receiver);
      receiver_mode_ = ConvertReceiverMode::kAny;
    } else {
      args_[0] = receiver;
    }
  }

  size_t count() const {
    if (receiver_mode_ == ConvertReceiverMode::kNullOrUndefined) {
      return args_.size();
    }
    return args_.size() - 1;
  }

  size_t count_with_receiver() const { return count() + 1; }

  ValueNode* operator[](size_t i) const {
    if (receiver_mode_ != ConvertReceiverMode::kNullOrUndefined) {
      i++;
    }
    if (i >= args_.size()) return nullptr;
    return args_[i];
  }

  void set_arg(size_t i, ValueNode* node) {
    if (receiver_mode_ != ConvertReceiverMode::kNullOrUndefined) {
      i++;
    }
    DCHECK_LT(i, args_.size());
    args_[i] = node;
  }

  Mode mode() const { return mode_; }

  ConvertReceiverMode receiver_mode() const { return receiver_mode_; }

  void Truncate(size_t new_args_count) {
    if (new_args_count >= count()) return;
    size_t args_to_pop = count() - new_args_count;
    for (size_t i = 0; i < args_to_pop; i++) {
      args_.pop_back();
    }
  }

  void PopReceiver(ConvertReceiverMode new_receiver_mode) {
    DCHECK_NE(receiver_mode_, ConvertReceiverMode::kNullOrUndefined);
    DCHECK_NE(new_receiver_mode, ConvertReceiverMode::kNullOrUndefined);
    DCHECK_GT(args_.size(), 0);  // We have at least a receiver to pop!
    // TODO(victorgomes): Do this better!
    for (size_t i = 0; i < args_.size() - 1; i++) {
      args_[i] = args_[i + 1];
    }
    args_.pop_back();

    // If there is no non-receiver argument to become the new receiver,
    // consider the new receiver to be known undefined.
    receiver_mode_ = args_.size() == 0 ? ConvertReceiverMode::kNullOrUndefined
                                       : new_receiver_mode;
  }

 private:
  ConvertReceiverMode receiver_mode_;
  base::SmallVector<ValueNode*, 8> args_;
  Mode mode_;
};

class V8_NODISCARD MaglevGraphBuilder::CallSpeculationScope {
 public:
  CallSpeculationScope(MaglevGraphBuilder* builder,
                       compiler::FeedbackSource feedback_source)
      : builder_(builder) {
    DCHECK(!builder_->current_speculation_feedback_.IsValid());
    if (feedback_source.IsValid()) {
      DCHECK_EQ(
          FeedbackNexus(feedback_source.vector, feedback_source.slot).kind(),
          FeedbackSlotKind::kCall);
    }
    builder_->current_speculation_feedback_ = feedback_source;
  }
  ~CallSpeculationScope() {
    builder_->current_speculation_feedback_ = compiler::FeedbackSource();
  }

 private:
  MaglevGraphBuilder* builder_;
};

class V8_NODISCARD MaglevGraphBuilder::DeoptFrameScope {
 public:
  DeoptFrameScope(MaglevGraphBuilder* builder, Builtin continuation,
                  compiler::OptionalJSFunctionRef maybe_js_target = {})
      : builder_(builder),
        parent_(builder->current_deopt_scope_),
        data_(DeoptFrame::BuiltinContinuationFrameData{
            continuation, {}, builder->GetContext(), maybe_js_target}) {
    builder_->current_deopt_scope_ = this;
  }

  DeoptFrameScope(MaglevGraphBuilder* builder, Builtin continuation,
                  compiler::OptionalJSFunctionRef maybe_js_target,
                  base::Vector<ValueNode* const> parameters)
      : builder_(builder),
        parent_(builder->current_deopt_scope_),
        data_(DeoptFrame::BuiltinContinuationFrameData{
            continuation, builder->zone()->CloneVector(parameters),
            builder->GetContext(), maybe_js_target}) {
    builder_->current_deopt_scope_ = this;
  }

  DeoptFrameScope(MaglevGraphBuilder* builder, ValueNode* receiver)
      : builder_(builder),
        parent_(builder->current_deopt_scope_),
        data_(DeoptFrame::ConstructInvokeStubFrameData{
            *builder->compilation_unit(), builder->current_source_position_,
            receiver, builder->GetContext()}) {
    builder_->current_deopt_scope_ = this;
  }

  ~DeoptFrameScope() { builder_->current_deopt_scope_ = parent_; }

  DeoptFrameScope* parent() const { return parent_; }

  DeoptFrame::FrameData data() const { return data_; }

 private:
  MaglevGraphBuilder* builder_;
  DeoptFrameScope* parent_;
  DeoptFrame::FrameData data_;
};

// Helper class for building a subgraph with its own control flow, that is not
// attached to any bytecode.
//
// It does this by creating a fake dummy compilation unit and frame state, and
// wrapping up all the places where it pretends to be interpreted but isn't.
class MaglevGraphBuilder::MaglevSubGraphBuilder {
 public:
  class Variable {
   public:
    explicit Variable(int index) : pseudo_register_(index) {}

   private:
    friend class MaglevSubGraphBuilder;

    // Variables pretend to be interpreter registers as far as the dummy
    // compilation unit and merge states are concerned.
    interpreter::Register pseudo_register_;
  };

  class Label {
   public:
    Label(MaglevSubGraphBuilder* sub_builder, int predecessor_count)
        : predecessor_count_(predecessor_count),
          liveness_(sub_builder->builder_->zone()
                        ->New<compiler::BytecodeLivenessState>(
                            sub_builder->compilation_unit_->register_count(),
                            sub_builder->builder_->zone())) {}
    Label(MaglevSubGraphBuilder* sub_builder, int predecessor_count,
          std::initializer_list<Variable*> vars)
        : Label(sub_builder, predecessor_count) {
      for (Variable* var : vars) {
        liveness_->MarkRegisterLive(var->pseudo_register_.index());
      }
    }

   private:
    explicit Label(MergePointInterpreterFrameState* merge_state,
                   BasicBlock* basic_block)
        : merge_state_(merge_state), ref_(basic_block) {}

    friend class MaglevSubGraphBuilder;
    MergePointInterpreterFrameState* merge_state_ = nullptr;
    int predecessor_count_ = -1;
    compiler::BytecodeLivenessState* liveness_ = nullptr;
    BasicBlockRef ref_;
  };

  class LoopLabel {
   public:
   private:
    explicit LoopLabel(MergePointInterpreterFrameState* merge_state,
                       BasicBlock* loop_header)
        : merge_state_(merge_state), loop_header_(loop_header) {}

    friend class MaglevSubGraphBuilder;
    MergePointInterpreterFrameState* merge_state_ = nullptr;
    BasicBlock* loop_header_;
  };

  explicit MaglevSubGraphBuilder(MaglevGraphBuilder* builder,
                                 int variable_count)
      : builder_(builder),
        compilation_unit_(MaglevCompilationUnit::NewDummy(
            builder->zone(), builder->compilation_unit(), variable_count, 0)),
        pseudo_frame_(*compilation_unit_, nullptr) {
    // We need to set a context, since this is unconditional in the frame state,
    // so set it to the real context.
    pseudo_frame_.set(interpreter::Register::current_context(),
                      builder_->current_interpreter_frame().get(
                          interpreter::Register::current_context()));
    DCHECK_NULL(pseudo_frame_.known_node_aspects());
  }

  LoopLabel BeginLoop(std::initializer_list<Variable*> loop_vars) {
    // Create fake liveness and loop info for the loop, with all given loop vars
    // set to be live and assigned inside the loop.
    compiler::BytecodeLivenessState* loop_header_liveness =
        builder_->zone()->New<compiler::BytecodeLivenessState>(
            compilation_unit_->register_count(), builder_->zone());
    compiler::LoopInfo* loop_info = builder_->zone()->New<compiler::LoopInfo>(
        -1, 0, kMaxInt, compilation_unit_->parameter_count(),
        compilation_unit_->register_count(), builder_->zone());
    for (Variable* var : loop_vars) {
      loop_header_liveness->MarkRegisterLive(var->pseudo_register_.index());
      loop_info->assignments().Add(var->pseudo_register_);
    }

    // Finish the current block, jumping (as a fallthrough) to the loop header.
    BasicBlockRef loop_header_ref;
    BasicBlock* loop_predecessor =
        builder_->FinishBlock<Jump>({}, &loop_header_ref);

    // Create a state for the loop header, with two predecessors (the above jump
    // and the back edge), and initialise with the current state.
    MergePointInterpreterFrameState* loop_state =
        MergePointInterpreterFrameState::NewForLoop(
            pseudo_frame_, *compilation_unit_, 0, 2, loop_header_liveness,
            loop_info);

    {
      BorrowParentKnownNodeAspects borrow(this);
      loop_state->Merge(builder_, *compilation_unit_, pseudo_frame_,
                        loop_predecessor);
    }

    // Start a new basic block for the loop.
    DCHECK_NULL(pseudo_frame_.known_node_aspects());
    pseudo_frame_.CopyFrom(*compilation_unit_, *loop_state);
    MoveKnownNodeAspectsToParent();

    builder_->ProcessMergePointPredecessors(*loop_state, loop_header_ref);
    builder_->StartNewBlock(nullptr, loop_state, loop_header_ref);

    return LoopLabel{loop_state, loop_header_ref.block_ptr()};
  }

  template <typename ControlNodeT, typename... Args>
  void GotoIfTrue(Label* true_target,
                  std::initializer_list<ValueNode*> control_inputs,
                  Args&&... args) {
    static_assert(IsConditionalControlNode(Node::opcode_of<ControlNodeT>));

    BasicBlockRef fallthrough_ref;

    // Pass through to FinishBlock, converting Labels to BasicBlockRefs and the
    // fallthrough label to the fallthrough ref.
    BasicBlock* block = builder_->FinishBlock<ControlNodeT>(
        control_inputs, std::forward<Args>(args)..., &true_target->ref_,
        &fallthrough_ref);

    MergeIntoLabel(true_target, block);

    builder_->StartNewBlock(block, nullptr, fallthrough_ref);
  }

  template <typename ControlNodeT, typename... Args>
  void GotoIfFalse(Label* false_target,
                   std::initializer_list<ValueNode*> control_inputs,
                   Args&&... args) {
    static_assert(IsConditionalControlNode(Node::opcode_of<ControlNodeT>));

    BasicBlockRef fallthrough_ref;

    // Pass through to FinishBlock, converting Labels to BasicBlockRefs and the
    // fallthrough label to the fallthrough ref.
    BasicBlock* block = builder_->FinishBlock<ControlNodeT>(
        control_inputs, std::forward<Args>(args)..., &fallthrough_ref,
        &false_target->ref_);

    MergeIntoLabel(false_target, block);

    builder_->StartNewBlock(block, nullptr, fallthrough_ref);
  }

  void Goto(Label* label) {
    if (builder_->current_block_ == nullptr) {
      label->merge_state_->MergeDead(*compilation_unit_);
      return;
    }

    BasicBlock* block = builder_->FinishBlock<Jump>({}, &label->ref_);
    MergeIntoLabel(label, block);
  }

  void EndLoop(LoopLabel* loop_label) {
    if (builder_->current_block_ == nullptr) {
      loop_label->merge_state_->MergeDeadLoop(*compilation_unit_);
      return;
    }

    BasicBlock* block =
        builder_->FinishBlock<JumpLoop>({}, loop_label->loop_header_);
    {
      BorrowParentKnownNodeAspects borrow(this);
      loop_label->merge_state_->MergeLoop(builder_, *compilation_unit_,
                                          pseudo_frame_, block);
    }
    block->set_predecessor_id(loop_label->merge_state_->predecessor_count() -
                              1);
  }

  void Bind(Label* label) {
    DCHECK_NULL(builder_->current_block_);

    DCHECK_NULL(pseudo_frame_.known_node_aspects());
    pseudo_frame_.CopyFrom(*compilation_unit_, *label->merge_state_);
    MoveKnownNodeAspectsToParent();

    builder_->ProcessMergePointPredecessors(*label->merge_state_, label->ref_);
    builder_->StartNewBlock(nullptr, label->merge_state_, label->ref_);
  }

  void set(Variable& var, ValueNode* value) {
    pseudo_frame_.set(var.pseudo_register_, value);
  }
  ValueNode* get(const Variable& var) const {
    return pseudo_frame_.get(var.pseudo_register_);
  }

 private:
  // Known node aspects for the pseudo frame are null aside from when merging --
  // before each merge, we should borrow the node aspects from the parent
  // builder, and after each merge point, we should copy the node aspects back
  // to the parent. This is so that the parent graph builder can update its own
  // known node aspects without having to worry about this pseudo frame.
  class BorrowParentKnownNodeAspects {
   public:
    explicit BorrowParentKnownNodeAspects(MaglevSubGraphBuilder* sub_builder)
        : sub_builder_(sub_builder) {
      sub_builder_->TakeKnownNodeAspectsFromParent();
    }
    ~BorrowParentKnownNodeAspects() {
      sub_builder_->MoveKnownNodeAspectsToParent();
    }

   private:
    MaglevSubGraphBuilder* sub_builder_;
  };

  void TakeKnownNodeAspectsFromParent() {
    DCHECK_NULL(pseudo_frame_.known_node_aspects());
    pseudo_frame_.set_known_node_aspects(
        builder_->current_interpreter_frame_.known_node_aspects());
  }

  void MoveKnownNodeAspectsToParent() {
    DCHECK_NOT_NULL(pseudo_frame_.known_node_aspects());
    builder_->current_interpreter_frame_.set_known_node_aspects(
        pseudo_frame_.known_node_aspects());
    pseudo_frame_.clear_known_node_aspects();
  }

  void MergeIntoLabel(Label* label, BasicBlock* predecessor) {
    BorrowParentKnownNodeAspects borrow(this);

    if (label->merge_state_ == nullptr) {
      // If there's no merge state, allocate a new one.
      label->merge_state_ = MergePointInterpreterFrameState::New(
          *compilation_unit_, pseudo_frame_, 0, label->predecessor_count_,
          predecessor, label->liveness_);
    } else {
      // If there already is a frame state, merge.
      label->merge_state_->Merge(builder_, *compilation_unit_, pseudo_frame_,
                                 predecessor);
    }
  }
  template <typename T>
  T LabelToRef(T&& val) {
    return std::forward<T>(val);
  }

  MaglevGraphBuilder* builder_;
  MaglevCompilationUnit* compilation_unit_;
  InterpreterFrameState pseudo_frame_;
};

MaglevGraphBuilder::MaglevGraphBuilder(LocalIsolate* local_isolate,
                                       MaglevCompilationUnit* compilation_unit,
                                       Graph* graph, float call_frequency,
                                       BytecodeOffset caller_bytecode_offset,
                                       int inlining_id,
                                       MaglevGraphBuilder* parent)
    : local_isolate_(local_isolate),
      compilation_unit_(compilation_unit),
      parent_(parent),
      graph_(graph),
      bytecode_analysis_(bytecode().object(), zone(),
                         compilation_unit->osr_offset(), true),
      iterator_(bytecode().object()),
      source_position_iterator_(bytecode().SourcePositionTable(broker())),
      allow_loop_peeling_(is_inline() ? parent_->allow_loop_peeling_
                                      : v8_flags.maglev_loop_peeling),
      decremented_predecessor_offsets_(zone()),
      loop_headers_to_peel_(bytecode().length(), zone()),
      call_frequency_(call_frequency),
      // Add an extra jump_target slot for the inline exit if needed.
      jump_targets_(zone()->AllocateArray<BasicBlockRef>(
          bytecode().length() + (is_inline() ? 1 : 0))),
      // Overallocate merge_states_ by one to allow always looking up the
      // next offset. This overallocated slot can also be used for the inline
      // exit when needed.
      merge_states_(zone()->AllocateArray<MergePointInterpreterFrameState*>(
          bytecode().length() + 1)),
      current_interpreter_frame_(
          *compilation_unit_,
          is_inline() ? parent->current_interpreter_frame_.known_node_aspects()
                      : compilation_unit_->zone()->New<KnownNodeAspects>(
                            compilation_unit_->zone())),
      caller_bytecode_offset_(caller_bytecode_offset),
      entrypoint_(compilation_unit->is_osr()
                      ? bytecode_analysis_.osr_entry_point()
                      : 0),
      inlining_id_(inlining_id),
      catch_block_stack_(zone()) {
  memset(merge_states_, 0,
         (bytecode().length() + 1) * sizeof(InterpreterFrameState*));
  // Default construct basic block refs.
  // TODO(leszeks): This could be a memset of nullptr to ..._jump_targets_.
  for (int i = 0; i < bytecode().length(); ++i) {
    new (&jump_targets_[i]) BasicBlockRef();
  }

  if (is_inline()) {
    DCHECK_NOT_NULL(parent_);
    DCHECK_GT(compilation_unit->inlining_depth(), 0);
    // The allocation/initialisation logic here relies on inline_exit_offset
    // being the offset one past the end of the bytecode.
    DCHECK_EQ(inline_exit_offset(), bytecode().length());
    merge_states_[inline_exit_offset()] = nullptr;
    new (&jump_targets_[inline_exit_offset()]) BasicBlockRef();
  }

  CHECK_IMPLIES(compilation_unit_->is_osr(), graph_->is_osr());
  CHECK_EQ(compilation_unit_->info()->toplevel_osr_offset() !=
               BytecodeOffset::None(),
           graph_->is_osr());
  if (compilation_unit_->is_osr()) {
    CHECK(!is_inline());
#ifdef DEBUG
    // OSR'ing into the middle of a loop is currently not supported. There
    // should not be any issue with OSR'ing outside of loops, just we currently
    // dont do it...
    iterator_.SetOffset(compilation_unit_->osr_offset().ToInt());
    DCHECK_EQ(iterator_.current_bytecode(), interpreter::Bytecode::kJumpLoop);
    DCHECK_EQ(entrypoint_, iterator_.GetJumpTargetOffset());
    iterator_.SetOffset(entrypoint_);
#endif

    if (v8_flags.trace_maglev_graph_building) {
      std::cerr << "- Non-standard entrypoint @" << entrypoint_
                << " by OSR from @" << compilation_unit_->osr_offset().ToInt()
                << std::endl;
    }
  }
  CHECK_IMPLIES(!compilation_unit_->is_osr(), entrypoint_ == 0);

  CalculatePredecessorCounts();
}

void MaglevGraphBuilder::StartPrologue() {
  current_block_ = zone()->New<BasicBlock>(nullptr, zone());
}

BasicBlock* MaglevGraphBuilder::EndPrologue() {
  BasicBlock* first_block = FinishBlock<Jump>({}, &jump_targets_[entrypoint_]);
  MergeIntoFrameState(first_block, entrypoint_);
  return first_block;
}

void MaglevGraphBuilder::SetArgument(int i, ValueNode* value) {
  interpreter::Register reg = interpreter::Register::FromParameterIndex(i);
  current_interpreter_frame_.set(reg, value);
}

ValueNode* MaglevGraphBuilder::GetTaggedArgument(int i) {
  interpreter::Register reg = interpreter::Register::FromParameterIndex(i);
  return GetTaggedValue(reg);
}

void MaglevGraphBuilder::InitializeRegister(interpreter::Register reg,
                                            ValueNode* value) {
  current_interpreter_frame_.set(
      reg, value ? value : AddNewNode<InitialValue>({}, reg));
}

void MaglevGraphBuilder::BuildRegisterFrameInitialization(
    ValueNode* context, ValueNode* closure, ValueNode* new_target) {
  if (closure == nullptr &&
      compilation_unit_->info()->specialize_to_function_context()) {
    compiler::JSFunctionRef function = compiler::MakeRefAssumeMemoryFence(
        broker(), broker()->CanonicalPersistentHandle(
                      compilation_unit_->info()->toplevel_function()));
    closure = GetConstant(function);
    context = GetConstant(function.context(broker()));
  }
  InitializeRegister(interpreter::Register::current_context(), context);
  InitializeRegister(interpreter::Register::function_closure(), closure);

  interpreter::Register new_target_or_generator_register =
      bytecode().incoming_new_target_or_generator_register();

  int register_index = 0;

  if (compilation_unit_->is_osr()) {
    for (; register_index < register_count(); register_index++) {
      auto val =
          AddNewNode<InitialValue>({}, interpreter::Register(register_index));
      InitializeRegister(interpreter::Register(register_index), val);
      graph_->osr_values().push_back(val);
    }
    return;
  }

  // TODO(leszeks): Don't emit if not needed.
  ValueNode* undefined_value = GetRootConstant(RootIndex::kUndefinedValue);
  if (new_target_or_generator_register.is_valid()) {
    int new_target_index = new_target_or_generator_register.index();
    for (; register_index < new_target_index; register_index++) {
      current_interpreter_frame_.set(interpreter::Register(register_index),
                                     undefined_value);
    }
    current_interpreter_frame_.set(
        new_target_or_generator_register,
        new_target ? new_target
                   : GetRegisterInput(kJavaScriptCallNewTargetRegister));
    register_index++;
  }
  for (; register_index < register_count(); register_index++) {
    InitializeRegister(interpreter::Register(register_index), undefined_value);
  }
}

void MaglevGraphBuilder::BuildMergeStates() {
  auto offset_and_info = bytecode_analysis().GetLoopInfos().begin();
  auto end = bytecode_analysis().GetLoopInfos().end();
  while (offset_and_info != end && offset_and_info->first < entrypoint_) {
    ++offset_and_info;
  }
  for (; offset_and_info != end; ++offset_and_info) {
    int offset = offset_and_info->first;
    const compiler::LoopInfo& loop_info = offset_and_info->second;
    if (loop_headers_to_peel_.Contains(offset)) {
      // Peeled loops are treated like normal merges at first. We will construct
      // the proper loop header merge state when reaching the `JumpLoop` of the
      // peeled iteration.
      continue;
    }
    const compiler::BytecodeLivenessState* liveness = GetInLivenessFor(offset);
    DCHECK_NULL(merge_states_[offset]);
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "- Creating loop merge state at @" << offset << std::endl;
    }
    merge_states_[offset] = MergePointInterpreterFrameState::NewForLoop(
        current_interpreter_frame_, *compilation_unit_, offset,
        NumPredecessors(offset), liveness, &loop_info);
  }

  if (bytecode().handler_table_size() > 0) {
    HandlerTable table(*bytecode().object());
    for (int i = 0; i < table.NumberOfRangeEntries(); i++) {
      const int offset = table.GetRangeHandler(i);
      const interpreter::Register context_reg(table.GetRangeData(i));
      const compiler::BytecodeLivenessState* liveness =
          GetInLivenessFor(offset);
      DCHECK_EQ(NumPredecessors(offset), 0);
      DCHECK_NULL(merge_states_[offset]);
      if (v8_flags.trace_maglev_graph_building) {
        std::cout << "- Creating exception merge state at @" << offset
                  << ", context register r" << context_reg.index() << std::endl;
      }
      merge_states_[offset] = MergePointInterpreterFrameState::NewForCatchBlock(
          *compilation_unit_, liveness, offset, context_reg, graph_);
    }
  }
}

namespace {

template <int index, interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper;

// Terminal cases
template <int index>
struct GetResultLocationAndSizeHelper<index> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    // TODO(leszeks): This should probably actually be "UNREACHABLE" but we have
    // lazy deopt info for interrupt budget updates at returns, not for actual
    // lazy deopts, but just for stack iteration purposes.
    return {interpreter::Register::invalid_value(), 0};
  }
  static bool HasOutputRegisterOperand() { return false; }
};

template <int index, interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper<index, interpreter::OperandType::kRegOut,
                                      operands...> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    // We shouldn't have any other output operands than this one.
    return {iterator.GetRegisterOperand(index), 1};
  }
  static bool HasOutputRegisterOperand() { return true; }
};

template <int index, interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper<
    index, interpreter::OperandType::kRegOutPair, operands...> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    // We shouldn't have any other output operands than this one.
    return {iterator.GetRegisterOperand(index), 2};
  }
  static bool HasOutputRegisterOperand() { return true; }
};

template <int index, interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper<
    index, interpreter::OperandType::kRegOutTriple, operands...> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    // We shouldn't have any other output operands than this one.
    DCHECK(!(GetResultLocationAndSizeHelper<
             index + 1, operands...>::HasOutputRegisterOperand()));
    return {iterator.GetRegisterOperand(index), 3};
  }
  static bool HasOutputRegisterOperand() { return true; }
};

// We don't support RegOutList for lazy deopts.
template <int index, interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper<
    index, interpreter::OperandType::kRegOutList, operands...> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    interpreter::RegisterList list = iterator.GetRegisterListOperand(index);
    return {list.first_register(), list.register_count()};
  }
  static bool HasOutputRegisterOperand() { return true; }
};

// Induction case.
template <int index, interpreter::OperandType operand,
          interpreter::OperandType... operands>
struct GetResultLocationAndSizeHelper<index, operand, operands...> {
  static std::pair<interpreter::Register, int> GetResultLocationAndSize(
      const interpreter::BytecodeArrayIterator& iterator) {
    return GetResultLocationAndSizeHelper<
        index + 1, operands...>::GetResultLocationAndSize(iterator);
  }
  static bool HasOutputRegisterOperand() {
    return GetResultLocationAndSizeHelper<
        index + 1, operands...>::HasOutputRegisterOperand();
  }
};

template <interpreter::Bytecode bytecode,
          interpreter::ImplicitRegisterUse implicit_use,
          interpreter::OperandType... operands>
std::pair<interpreter::Register, int> GetResultLocationAndSizeForBytecode(
    const interpreter::BytecodeArrayIterator& iterator) {
  // We don't support output registers for implicit registers.
  DCHECK(!interpreter::BytecodeOperands::WritesImplicitRegister(implicit_use));
  if (interpreter::BytecodeOperands::WritesAccumulator(implicit_use)) {
    // If we write the accumulator, we shouldn't also write an output register.
    DCHECK(!(GetResultLocationAndSizeHelper<
             0, operands...>::HasOutputRegisterOperand()));
    return {interpreter::Register::virtual_accumulator(), 1};
  }

  // Use template magic to output a the appropriate GetRegisterOperand call and
  // size for this bytecode.
  return GetResultLocationAndSizeHelper<
      0, operands...>::GetResultLocationAndSize(iterator);
}

}  // namespace

std::pair<interpreter::Register, int>
MaglevGraphBuilder::GetResultLocationAndSize() const {
  using Bytecode = interpreter::Bytecode;
  using OperandType = interpreter::OperandType;
  using ImplicitRegisterUse = interpreter::ImplicitRegisterUse;
  Bytecode bytecode = iterator_.current_bytecode();
  // TODO(leszeks): Only emit these cases for bytecodes we know can lazy deopt.
  switch (bytecode) {
#define CASE(Name, ...)                                           \
  case Bytecode::k##Name:                                         \
    return GetResultLocationAndSizeForBytecode<Bytecode::k##Name, \
                                               __VA_ARGS__>(iterator_);
    BYTECODE_LIST(CASE)
#undef CASE
  }
  UNREACHABLE();
}

#ifdef DEBUG
bool MaglevGraphBuilder::HasOutputRegister(interpreter::Register reg) const {
  interpreter::Bytecode bytecode = iterator_.current_bytecode();
  if (reg == interpreter::Register::virtual_accumulator()) {
    return interpreter::Bytecodes::WritesAccumulator(bytecode);
  }
  for (int i = 0; i < interpreter::Bytecodes::NumberOfOperands(bytecode); ++i) {
    if (interpreter::Bytecodes::IsRegisterOutputOperandType(
            interpreter::Bytecodes::GetOperandType(bytecode, i))) {
      interpreter::Register operand_reg = iterator_.GetRegisterOperand(i);
      int operand_range = iterator_.GetRegisterOperandRange(i);
      if (base::IsInRange(reg.index(), operand_reg.index(),
                          operand_reg.index() + operand_range)) {
        return true;
      }
    }
  }
  return false;
}
#endif

DeoptFrame* MaglevGraphBuilder::GetParentDeoptFrame() {
  if (parent_ == nullptr) return nullptr;
  if (parent_deopt_frame_ == nullptr) {
    // The parent resumes after the call, which is roughly equivalent to a lazy
    // deopt. Use the helper function directly so that we can mark the
    // accumulator as dead (since it'll be overwritten by this function's
    // return value anyway).
    // TODO(leszeks): This is true for our current set of
    // inlinings/continuations, but there might be cases in the future where it
    // isn't. We may need to store the relevant overwritten register in
    // LazyDeoptFrameScope.
    DCHECK(interpreter::Bytecodes::WritesAccumulator(
        parent_->iterator_.current_bytecode()));

    parent_deopt_frame_ =
        zone()->New<DeoptFrame>(parent_->GetDeoptFrameForLazyDeoptHelper(
            parent_->current_deopt_scope_, true));
    if (inlined_arguments_) {
      parent_deopt_frame_ = zone()->New<InlinedArgumentsDeoptFrame>(
          *compilation_unit_, caller_bytecode_offset_, GetClosure(),
          *inlined_arguments_, parent_deopt_frame_);
    }
  }
  return parent_deopt_frame_;
}

DeoptFrame MaglevGraphBuilder::GetLatestCheckpointedFrame() {
  if (!latest_checkpointed_frame_) {
    latest_checkpointed_frame_.emplace(InterpretedDeoptFrame(
        *compilation_unit_,
        zone()->New<CompactInterpreterFrameState>(
            *compilation_unit_, GetInLiveness(), current_interpreter_frame_),
        GetClosure(), BytecodeOffset(iterator_.current_offset()),
        current_source_position_, GetParentDeoptFrame()));

    if (current_deopt_scope_ != nullptr) {
      // Support exactly one eager deopt builtin continuation. This can be
      // expanded in the future if necessary.
      DCHECK_NULL(current_deopt_scope_->parent());
      DCHECK_EQ(current_deopt_scope_->data().tag(),
                DeoptFrame::FrameType::kBuiltinContinuationFrame);

      // Wrap the above frame in the scope frame.
      latest_checkpointed_frame_.emplace(
          current_deopt_scope_->data(),
          zone()->New<DeoptFrame>(*latest_checkpointed_frame_));
    }
  }
  return *latest_checkpointed_frame_;
}

DeoptFrame MaglevGraphBuilder::GetDeoptFrameForLazyDeopt() {
  return GetDeoptFrameForLazyDeoptHelper(current_deopt_scope_, false);
}

DeoptFrame MaglevGraphBuilder::GetDeoptFrameForLazyDeoptHelper(
    DeoptFrameScope* scope, bool mark_accumulator_dead) {
  if (scope == nullptr) {
    // Potentially copy the out liveness if we want to explicitly drop the
    // accumulator.
    const compiler::BytecodeLivenessState* liveness = GetOutLiveness();
    if (mark_accumulator_dead && liveness->AccumulatorIsLive()) {
      compiler::BytecodeLivenessState* liveness_copy =
          zone()->New<compiler::BytecodeLivenessState>(*liveness, zone());
      liveness_copy->MarkAccumulatorDead();
      liveness = liveness_copy;
    }
    return InterpretedDeoptFrame(
        *compilation_unit_,
        zone()->New<CompactInterpreterFrameState>(*compilation_unit_, liveness,
                                                  current_interpreter_frame_),
        GetClosure(), BytecodeOffset(iterator_.current_offset()),
        current_source_position_, GetParentDeoptFrame());
  }

  // Currently only support builtin continuations for bytecodes that write to
  // the accumulator
  DCHECK(
      interpreter::Bytecodes::WritesAccumulator(iterator_.current_bytecode()));
  // Mark the accumulator dead in parent frames since we know that the
  // continuation will write it.
  return DeoptFrame(scope->data(),
                    zone()->New<DeoptFrame>(GetDeoptFrameForLazyDeoptHelper(
                        scope->parent(),
                        scope->data().tag() ==
                            DeoptFrame::FrameType::kBuiltinContinuationFrame)));
}

InterpretedDeoptFrame MaglevGraphBuilder::GetDeoptFrameForEntryStackCheck() {
  DCHECK_EQ(iterator_.current_offset(), entrypoint_);
  DCHECK_NULL(parent_);
  int osr_jump_loop_pos;
  if (graph_->is_osr()) {
    osr_jump_loop_pos = bytecode_analysis_.osr_bailout_id().ToInt();
  }
  return InterpretedDeoptFrame(
      *compilation_unit_,
      zone()->New<CompactInterpreterFrameState>(
          *compilation_unit_,
          GetInLivenessFor(graph_->is_osr() ? osr_jump_loop_pos : 0),
          current_interpreter_frame_),
      GetClosure(),
      BytecodeOffset(graph_->is_osr() ? osr_jump_loop_pos
                                      : kFunctionEntryBytecodeOffset),
      current_source_position_, nullptr);
}

ValueNode* MaglevGraphBuilder::GetTaggedValue(
    ValueNode* value, UseReprHintRecording record_use_repr_hint) {
  if (V8_LIKELY(record_use_repr_hint == UseReprHintRecording::kRecord)) {
    RecordUseReprHintIfPhi(value, UseRepresentation::kTagged);
  }

  ValueRepresentation representation =
      value->properties().value_representation();
  if (representation == ValueRepresentation::kTagged) return value;

  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(value);
  auto& alternative = node_info->alternative();

  if (ValueNode* alt = alternative.tagged()) {
    return alt;
  }

  switch (representation) {
    case ValueRepresentation::kInt32: {
      if (NodeTypeIsSmi(node_info->type())) {
        return alternative.set_tagged(AddNewNode<UnsafeSmiTag>({value}));
      }
      return alternative.set_tagged(AddNewNode<Int32ToNumber>({value}));
    }
    case ValueRepresentation::kUint32: {
      if (NodeTypeIsSmi(node_info->type())) {
        return alternative.set_tagged(AddNewNode<UnsafeSmiTag>({value}));
      }
      return alternative.set_tagged(AddNewNode<Uint32ToNumber>({value}));
    }
    case ValueRepresentation::kFloat64: {
      return alternative.set_tagged(AddNewNode<Float64ToTagged>({value}));
    }
    case ValueRepresentation::kHoleyFloat64: {
      return alternative.set_tagged(AddNewNode<HoleyFloat64ToTagged>({value}));
    }

    case ValueRepresentation::kTagged:
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
  UNREACHABLE();
}

ValueNode* MaglevGraphBuilder::GetSmiValue(
    ValueNode* value, UseReprHintRecording record_use_repr_hint) {
  if (V8_LIKELY(record_use_repr_hint == UseReprHintRecording::kRecord)) {
    RecordUseReprHintIfPhi(value, UseRepresentation::kTagged);
  }

  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(value);

  ValueRepresentation representation =
      value->properties().value_representation();
  if (representation == ValueRepresentation::kTagged) {
    BuildCheckSmi(value, !value->Is<Phi>());
    return value;
  }

  auto& alternative = node_info->alternative();

  if (ValueNode* alt = alternative.tagged()) {
    BuildCheckSmi(alt, !value->Is<Phi>());
    return alt;
  }

  switch (representation) {
    case ValueRepresentation::kInt32: {
      if (NodeTypeIsSmi(node_info->type())) {
        return alternative.set_tagged(AddNewNode<UnsafeSmiTag>({value}));
      }
      return alternative.set_tagged(AddNewNode<CheckedSmiTagInt32>({value}));
    }
    case ValueRepresentation::kUint32: {
      if (NodeTypeIsSmi(node_info->type())) {
        return alternative.set_tagged(AddNewNode<UnsafeSmiTag>({value}));
      }
      return alternative.set_tagged(AddNewNode<CheckedSmiTagUint32>({value}));
    }
    case ValueRepresentation::kFloat64: {
      return alternative.set_tagged(AddNewNode<CheckedSmiTagFloat64>({value}));
    }
    case ValueRepresentation::kHoleyFloat64: {
      return alternative.set_tagged(AddNewNode<CheckedSmiTagFloat64>({value}));
    }

    case ValueRepresentation::kTagged:
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
  UNREACHABLE();
}

namespace {
CheckType GetCheckType(NodeType type) {
  return NodeTypeIs(type, NodeType::kAnyHeapObject)
             ? CheckType::kOmitHeapObjectCheck
             : CheckType::kCheckHeapObject;
}
}  // namespace

ValueNode* MaglevGraphBuilder::GetInternalizedString(
    interpreter::Register reg) {
  ValueNode* node = GetTaggedValue(reg);
  NodeType old_type;
  if (CheckType(node, NodeType::kInternalizedString, &old_type)) return node;
  if (!NodeTypeIs(old_type, NodeType::kString)) {
    NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(node);
    known_info->CombineType(NodeType::kString);
  }
  node = AddNewNode<CheckedInternalizedString>({node}, GetCheckType(old_type));
  current_interpreter_frame_.set(reg, node);
  return node;
}

namespace {
NodeType ToNumberHintToNodeType(ToNumberHint conversion_type) {
  switch (conversion_type) {
    case ToNumberHint::kAssumeSmi:
      return NodeType::kSmi;
    case ToNumberHint::kDisallowToNumber:
    case ToNumberHint::kAssumeNumber:
      return NodeType::kNumber;
    case ToNumberHint::kAssumeNumberOrOddball:
      return NodeType::kNumberOrOddball;
  }
}
TaggedToFloat64ConversionType ToNumberHintToConversionType(
    ToNumberHint conversion_type) {
  switch (conversion_type) {
    case ToNumberHint::kAssumeSmi:
      UNREACHABLE();
    case ToNumberHint::kDisallowToNumber:
    case ToNumberHint::kAssumeNumber:
      return TaggedToFloat64ConversionType::kOnlyNumber;
    case ToNumberHint::kAssumeNumberOrOddball:
      return TaggedToFloat64ConversionType::kNumberOrOddball;
  }
}
}  // namespace

ValueNode* MaglevGraphBuilder::GetTruncatedInt32ForToNumber(ValueNode* value,
                                                            ToNumberHint hint) {
  RecordUseReprHintIfPhi(value, UseRepresentation::kTruncatedInt32);

  ValueRepresentation representation =
      value->properties().value_representation();
  if (representation == ValueRepresentation::kInt32) return value;
  if (representation == ValueRepresentation::kUint32) {
    // This node is cheap (no code gen, just a bitcast), so don't cache it.
    return AddNewNode<TruncateUint32ToInt32>({value});
  }

  // Process constants first to avoid allocating NodeInfo for them.
  switch (value->opcode()) {
    case Opcode::kConstant: {
      compiler::ObjectRef object = value->Cast<Constant>()->object();
      if (!object.IsHeapNumber()) break;
      int32_t truncated_value = DoubleToInt32(object.AsHeapNumber().value());
      if (!Smi::IsValid(truncated_value)) break;
      return GetInt32Constant(truncated_value);
    }
    case Opcode::kSmiConstant:
      return GetInt32Constant(value->Cast<SmiConstant>()->value().value());
    case Opcode::kRootConstant: {
      Object root_object =
          local_isolate_->root(value->Cast<RootConstant>()->index());
      if (!IsOddball(root_object, local_isolate_)) break;
      int32_t truncated_value =
          DoubleToInt32(Oddball::cast(root_object)->to_number_raw());
      // All oddball ToNumber truncations are valid Smis.
      DCHECK(Smi::IsValid(truncated_value));
      return GetInt32Constant(truncated_value);
    }
    case Opcode::kFloat64Constant: {
      int32_t truncated_value =
          DoubleToInt32(value->Cast<Float64Constant>()->value().get_scalar());
      if (!Smi::IsValid(truncated_value)) break;
      return GetInt32Constant(truncated_value);
    }

    // We could emit unconditional eager deopts for other kinds of constant, but
    // it's not necessary, the appropriate checking conversion nodes will deopt.
    default:
      break;
  }

  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(value);
  auto& alternative = node_info->alternative();

  // If there is an int32_alternative, then that works as a truncated value
  // too.
  if (ValueNode* alt = alternative.int32()) {
    return alt;
  }
  if (ValueNode* alt = alternative.truncated_int32_to_number()) {
    return alt;
  }

  switch (representation) {
    case ValueRepresentation::kTagged: {
      NodeType old_type;
      NodeType desired_type = ToNumberHintToNodeType(hint);
      EnsureType(value, desired_type, &old_type);
      if (NodeTypeIsSmi(old_type)) {
        // Smi untagging can be cached as an int32 alternative, not just a
        // truncated alternative.
        return alternative.set_int32(AddNewNode<UnsafeSmiUntag>({value}));
      }
      if (desired_type == NodeType::kSmi) {
        return alternative.set_int32(AddNewNode<CheckedSmiUntag>({value}));
      }
      TaggedToFloat64ConversionType conversion_type =
          ToNumberHintToConversionType(hint);
      if (NodeTypeIs(old_type, desired_type)) {
        return alternative.set_truncated_int32_to_number(
            AddNewNode<TruncateNumberOrOddballToInt32>({value},
                                                       conversion_type));
      }
      return alternative.set_truncated_int32_to_number(
          AddNewNode<CheckedTruncateNumberOrOddballToInt32>({value},
                                                            conversion_type));
    }
    case ValueRepresentation::kFloat64:
    // Ignore conversion_type for HoleyFloat64, and treat them like Float64.
    // ToNumber of undefined is anyway a NaN, so we'll simply truncate away
    // the NaN-ness of the hole, and don't need to do extra oddball checks so
    // we can ignore the hint (though we'll miss updating the feedback).
    case ValueRepresentation::kHoleyFloat64: {
      return alternative.set_truncated_int32_to_number(
          AddNewNode<TruncateFloat64ToInt32>({value}));
    }

    case ValueRepresentation::kInt32:
    case ValueRepresentation::kUint32:
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
  UNREACHABLE();
}

ValueNode* MaglevGraphBuilder::GetInt32(ValueNode* value) {
  RecordUseReprHintIfPhi(value, UseRepresentation::kInt32);

  ValueRepresentation representation =
      value->properties().value_representation();
  if (representation == ValueRepresentation::kInt32) return value;

  // Process constants first to avoid allocating NodeInfo for them.
  switch (value->opcode()) {
    case Opcode::kSmiConstant:
      return GetInt32Constant(value->Cast<SmiConstant>()->value().value());
    case Opcode::kFloat64Constant: {
      double double_value =
          value->Cast<Float64Constant>()->value().get_scalar();
      if (!IsSmiDouble(double_value)) break;
      return GetInt32Constant(
          FastD2I(value->Cast<Float64Constant>()->value().get_scalar()));
    }

    // We could emit unconditional eager deopts for other kinds of constant, but
    // it's not necessary, the appropriate checking conversion nodes will deopt.
    default:
      break;
  }

  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(value);
  auto& alternative = node_info->alternative();

  if (ValueNode* alt = alternative.int32()) {
    return alt;
  }

  switch (representation) {
    case ValueRepresentation::kTagged: {
      // TODO(leszeks): Widen this path to allow HeapNumbers with Int32 values.
      return alternative.set_int32(BuildSmiUntag(value));
    }
    case ValueRepresentation::kUint32: {
      if (node_info->is_smi()) {
        return alternative.set_int32(
            AddNewNode<TruncateUint32ToInt32>({value}));
      }
      return alternative.set_int32(AddNewNode<CheckedUint32ToInt32>({value}));
    }
    case ValueRepresentation::kFloat64:
    // The check here will also work for the hole NaN, so we can treat
    // HoleyFloat64 as Float64.
    case ValueRepresentation::kHoleyFloat64: {
      return alternative.set_int32(
          AddNewNode<CheckedTruncateFloat64ToInt32>({value}));
    }

    case ValueRepresentation::kInt32:
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
  UNREACHABLE();
}

ValueNode* MaglevGraphBuilder::GetFloat64(ValueNode* value) {
  RecordUseReprHintIfPhi(value, UseRepresentation::kFloat64);

  return GetFloat64ForToNumber(value, ToNumberHint::kDisallowToNumber);
}

ValueNode* MaglevGraphBuilder::GetFloat64ForToNumber(ValueNode* value,
                                                     ToNumberHint hint) {
  ValueRepresentation representation =
      value->properties().value_representation();
  if (representation == ValueRepresentation::kFloat64) return value;

  // Process constants first to avoid allocating NodeInfo for them.
  switch (value->opcode()) {
    case Opcode::kConstant: {
      compiler::ObjectRef object = value->Cast<Constant>()->object();
      if (object.IsHeapNumber()) {
        return GetFloat64Constant(object.AsHeapNumber().value());
      }
      // Oddballs should be RootConstants.
      DCHECK(!IsOddball(*object.object()));
      break;
    }
    case Opcode::kSmiConstant:
      return GetFloat64Constant(value->Cast<SmiConstant>()->value().value());
    case Opcode::kInt32Constant:
      return GetFloat64Constant(value->Cast<Int32Constant>()->value());
    case Opcode::kRootConstant: {
      Object root_object =
          local_isolate_->root(value->Cast<RootConstant>()->index());
      if (hint != ToNumberHint::kDisallowToNumber && IsOddball(root_object)) {
        return GetFloat64Constant(Oddball::cast(root_object)->to_number_raw());
      }
      if (IsHeapNumber(root_object)) {
        return GetFloat64Constant(HeapNumber::cast(root_object)->value());
      }
      break;
    }

    // We could emit unconditional eager deopts for other kinds of constant, but
    // it's not necessary, the appropriate checking conversion nodes will deopt.
    default:
      break;
  }

  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(value);
  auto& alternative = node_info->alternative();

  if (ValueNode* alt = alternative.float64()) {
    return alt;
  }

  switch (representation) {
    case ValueRepresentation::kTagged: {
      switch (hint) {
        case ToNumberHint::kAssumeSmi:
          // Get the float64 value of a Smi value its int32 representation.
          return GetFloat64(GetInt32(value));
        case ToNumberHint::kDisallowToNumber:
        case ToNumberHint::kAssumeNumber:
          // Number->Float64 conversions are exact alternatives, so they can
          // also become the canonical float64_alternative.
          return alternative.set_float64(BuildNumberOrOddballToFloat64(
              value, TaggedToFloat64ConversionType::kOnlyNumber));
        case ToNumberHint::kAssumeNumberOrOddball: {
          // NumberOrOddball->Float64 conversions are not exact alternatives,
          // since they lose the information that this is an oddball, so they
          // can only become the canonical float64_alternative if they are a
          // known number (and therefore not oddball).
          ValueNode* float64_node = BuildNumberOrOddballToFloat64(
              value, TaggedToFloat64ConversionType::kNumberOrOddball);
          if (NodeTypeIsNumber(node_info->type())) {
            alternative.set_float64(float64_node);
          }
          return float64_node;
        }
      }
    }
    case ValueRepresentation::kInt32:
      return alternative.set_float64(AddNewNode<ChangeInt32ToFloat64>({value}));
    case ValueRepresentation::kUint32:
      return alternative.set_float64(
          AddNewNode<ChangeUint32ToFloat64>({value}));
    case ValueRepresentation::kHoleyFloat64: {
      switch (hint) {
        case ToNumberHint::kAssumeSmi:
        case ToNumberHint::kDisallowToNumber:
        case ToNumberHint::kAssumeNumber:
          // Number->Float64 conversions are exact alternatives, so they can
          // also become the canonical float64_alternative.
          return alternative.set_float64(
              AddNewNode<CheckedHoleyFloat64ToFloat64>({value}));
        case ToNumberHint::kAssumeNumberOrOddball:
          // NumberOrOddball->Float64 conversions are not exact alternatives,
          // since they lose the information that this is an oddball, so they
          // cannot become the canonical float64_alternative.
          return AddNewNode<HoleyFloat64ToMaybeNanFloat64>({value});
      }
    }
    case ValueRepresentation::kFloat64:
    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }
  UNREACHABLE();
}

ValueNode* MaglevGraphBuilder::GetHoleyFloat64ForToNumber(ValueNode* value,
                                                          ToNumberHint hint) {
  RecordUseReprHintIfPhi(value, UseRepresentation::kHoleyFloat64);

  ValueRepresentation representation =
      value->properties().value_representation();
  // Ignore the hint for
  if (representation == ValueRepresentation::kHoleyFloat64) return value;
  return GetFloat64ForToNumber(value, hint);
}

namespace {
int32_t ClampToUint8(int32_t value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}
}  // namespace

ValueNode* MaglevGraphBuilder::GetUint8ClampedForToNumber(ValueNode* value,
                                                          ToNumberHint hint) {
  switch (value->properties().value_representation()) {
    case ValueRepresentation::kWord64:
      UNREACHABLE();
    case ValueRepresentation::kTagged: {
      if (SmiConstant* constant = value->TryCast<SmiConstant>()) {
        return GetInt32Constant(ClampToUint8(constant->value().value()));
      }
      NodeInfo* info = known_node_aspects().TryGetInfoFor(value);
      if (info && info->alternative().int32()) {
        return AddNewNode<Int32ToUint8Clamped>({info->alternative().int32()});
      }
      return AddNewNode<CheckedNumberToUint8Clamped>({value});
    }
    // Ignore conversion_type for HoleyFloat64, and treat them like Float64.
    // ToNumber of undefined is anyway a NaN, so we'll simply truncate away the
    // NaN-ness of the hole, and don't need to do extra oddball checks so we can
    // ignore the hint (though we'll miss updating the feedback).
    case ValueRepresentation::kFloat64:
    case ValueRepresentation::kHoleyFloat64:
      // TODO(leszeks): Handle Float64Constant, which requires the correct
      // rounding for clamping.
      return AddNewNode<Float64ToUint8Clamped>({value});
    case ValueRepresentation::kInt32:
      if (Int32Constant* constant = value->TryCast<Int32Constant>()) {
        return GetInt32Constant(ClampToUint8(constant->value()));
      }
      return AddNewNode<Int32ToUint8Clamped>({value});
    case ValueRepresentation::kUint32:
      return AddNewNode<Uint32ToUint8Clamped>({value});
  }
  UNREACHABLE();
}

namespace {
template <Operation kOperation>
struct NodeForOperationHelper;

#define NODE_FOR_OPERATION_HELPER(Name)               \
  template <>                                         \
  struct NodeForOperationHelper<Operation::k##Name> { \
    using generic_type = Generic##Name;               \
  };
OPERATION_LIST(NODE_FOR_OPERATION_HELPER)
#undef NODE_FOR_OPERATION_HELPER

template <Operation kOperation>
using GenericNodeForOperation =
    typename NodeForOperationHelper<kOperation>::generic_type;

// Bitwise operations reinterprets the numeric input as Int32 bits for a
// bitwise operation, which means we want to do slightly different conversions.
template <Operation kOperation>
constexpr bool BinaryOperationIsBitwiseInt32() {
  switch (kOperation) {
    case Operation::kBitwiseNot:
    case Operation::kBitwiseAnd:
    case Operation::kBitwiseOr:
    case Operation::kBitwiseXor:
    case Operation::kShiftLeft:
    case Operation::kShiftRight:
    case Operation::kShiftRightLogical:
      return true;
    default:
      return false;
  }
}
}  // namespace

// MAP_OPERATION_TO_NODES are tuples with the following format:
// - Operation name,
// - Int32 operation node,
// - Identity of int32 operation (e.g, 0 for add/sub and 1 for mul/div), if it
//   exists, or otherwise {}.
#define MAP_BINARY_OPERATION_TO_INT32_NODE(V) \
  V(Add, Int32AddWithOverflow, 0)             \
  V(Subtract, Int32SubtractWithOverflow, 0)   \
  V(Multiply, Int32MultiplyWithOverflow, 1)   \
  V(Divide, Int32DivideWithOverflow, 1)       \
  V(Modulus, Int32ModulusWithOverflow, {})    \
  V(BitwiseAnd, Int32BitwiseAnd, ~0)          \
  V(BitwiseOr, Int32BitwiseOr, 0)             \
  V(BitwiseXor, Int32BitwiseXor, 0)           \
  V(ShiftLeft, Int32ShiftLeft, 0)             \
  V(ShiftRight, Int32ShiftRight, 0)           \
  V(ShiftRightLogical, Int32ShiftRightLogical, {})

#define MAP_UNARY_OPERATION_TO_INT32_NODE(V) \
  V(BitwiseNot, Int32BitwiseNot)             \
  V(Increment, Int32IncrementWithOverflow)   \
  V(Decrement, Int32DecrementWithOverflow)   \
  V(Negate, Int32NegateWithOverflow)

#define MAP_COMPARE_OPERATION_TO_INT32_NODE(V) \
  V(Equal, Int32Equal)                         \
  V(StrictEqual, Int32StrictEqual)             \
  V(LessThan, Int32LessThan)                   \
  V(LessThanOrEqual, Int32LessThanOrEqual)     \
  V(GreaterThan, Int32GreaterThan)             \
  V(GreaterThanOrEqual, Int32GreaterThanOrEqual)

// MAP_OPERATION_TO_FLOAT64_NODE are tuples with the following format:
// (Operation name, Float64 operation node).
#define MAP_OPERATION_TO_FLOAT64_NODE(V) \
  V(Add, Float64Add)                     \
  V(Subtract, Float64Subtract)           \
  V(Multiply, Float64Multiply)           \
  V(Divide, Float64Divide)               \
  V(Modulus, Float64Modulus)             \
  V(Negate, Float64Negate)               \
  V(Exponentiate, Float64Exponentiate)

#define MAP_COMPARE_OPERATION_TO_FLOAT64_NODE(V) \
  V(Equal, Float64Equal)                         \
  V(StrictEqual, Float64StrictEqual)             \
  V(LessThan, Float64LessThan)                   \
  V(LessThanOrEqual, Float64LessThanOrEqual)     \
  V(GreaterThan, Float64GreaterThan)             \
  V(GreaterThanOrEqual, Float64GreaterThanOrEqual)

template <Operation kOperation>
static constexpr base::Optional<int> Int32Identity() {
  switch (kOperation) {
#define CASE(op, _, identity) \
  case Operation::k##op:      \
    return identity;
    MAP_BINARY_OPERATION_TO_INT32_NODE(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}

namespace {
template <Operation kOperation>
struct Int32NodeForHelper;
#define SPECIALIZATION(op, OpNode, ...)         \
  template <>                                   \
  struct Int32NodeForHelper<Operation::k##op> { \
    using type = OpNode;                        \
  };
MAP_UNARY_OPERATION_TO_INT32_NODE(SPECIALIZATION)
MAP_BINARY_OPERATION_TO_INT32_NODE(SPECIALIZATION)
MAP_COMPARE_OPERATION_TO_INT32_NODE(SPECIALIZATION)
#undef SPECIALIZATION

template <Operation kOperation>
using Int32NodeFor = typename Int32NodeForHelper<kOperation>::type;

template <Operation kOperation>
struct Float64NodeForHelper;
#define SPECIALIZATION(op, OpNode)                \
  template <>                                     \
  struct Float64NodeForHelper<Operation::k##op> { \
    using type = OpNode;                          \
  };
MAP_OPERATION_TO_FLOAT64_NODE(SPECIALIZATION)
MAP_COMPARE_OPERATION_TO_FLOAT64_NODE(SPECIALIZATION)
#undef SPECIALIZATION

template <Operation kOperation>
using Float64NodeFor = typename Float64NodeForHelper<kOperation>::type;
}  // namespace

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericUnaryOperationNode() {
  FeedbackSlot slot_index = GetSlotOperand(0);
  ValueNode* value = GetAccumulatorTagged();
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {value}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericBinaryOperationNode() {
  ValueNode* left = LoadRegisterTagged(0);
  ValueNode* right = GetAccumulatorTagged();
  FeedbackSlot slot_index = GetSlotOperand(1);
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {left, right}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildGenericBinarySmiOperationNode() {
  ValueNode* left = GetAccumulatorTagged();
  int constant = iterator_.GetImmediateOperand(0);
  ValueNode* right = GetSmiConstant(constant);
  FeedbackSlot slot_index = GetSlotOperand(1);
  SetAccumulator(AddNewNode<GenericNodeForOperation<kOperation>>(
      {left, right}, compiler::FeedbackSource{feedback(), slot_index}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildInt32UnaryOperationNode() {
  // Use BuildTruncatingInt32BitwiseNotForToNumber with Smi input hint
  // for truncating operations.
  static_assert(!BinaryOperationIsBitwiseInt32<kOperation>());
  // TODO(v8:7700): Do constant folding.
  ValueNode* value = GetAccumulatorInt32();
  using OpNodeT = Int32NodeFor<kOperation>;
  SetAccumulator(AddNewNode<OpNodeT>({value}));
}

void MaglevGraphBuilder::BuildTruncatingInt32BitwiseNotForToNumber(
    ToNumberHint hint) {
  // TODO(v8:7700): Do constant folding.
  ValueNode* value = GetTruncatedInt32ForToNumber(
      current_interpreter_frame_.accumulator(), hint);
  SetAccumulator(AddNewNode<Int32BitwiseNot>({value}));
}

template <Operation kOperation>
ValueNode* MaglevGraphBuilder::TryFoldInt32BinaryOperation(ValueNode* left,
                                                           ValueNode* right) {
  switch (kOperation) {
    case Operation::kModulus:
      // Note the `x % x = 0` fold is invalid since for negative x values the
      // result is -0.0.
      // TODO(v8:7700): Consider re-enabling this fold if the result is used
      // only in contexts where -0.0 is semantically equivalent to 0.0, or if x
      // is known to be non-negative.
    default:
      // TODO(victorgomes): Implement more folds.
      break;
  }
  return nullptr;
}

template <Operation kOperation>
ValueNode* MaglevGraphBuilder::TryFoldInt32BinaryOperation(ValueNode* left,
                                                           int right) {
  switch (kOperation) {
    case Operation::kModulus:
      // Note the `x % 1 = 0` and `x % -1 = 0` folds are invalid since for
      // negative x values the result is -0.0.
      // TODO(v8:7700): Consider re-enabling this fold if the result is used
      // only in contexts where -0.0 is semantically equivalent to 0.0, or if x
      // is known to be non-negative.
      // TODO(victorgomes): We can emit faster mod operation if {right} is power
      // of 2, unfortunately we need to know if {left} is negative or not.
      // Maybe emit a Int32ModulusRightIsPowerOf2?
    default:
      // TODO(victorgomes): Implement more folds.
      break;
  }
  return nullptr;
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildInt32BinaryOperationNode() {
  // Use BuildTruncatingInt32BinaryOperationNodeForToNumber with Smi input hint
  // for truncating operations.
  static_assert(!BinaryOperationIsBitwiseInt32<kOperation>());
  // TODO(v8:7700): Do constant folding.
  ValueNode* left = LoadRegisterInt32(0);
  ValueNode* right = GetAccumulatorInt32();

  if (ValueNode* result =
          TryFoldInt32BinaryOperation<kOperation>(left, right)) {
    SetAccumulator(result);
    return;
  }
  using OpNodeT = Int32NodeFor<kOperation>;

  SetAccumulator(AddNewNode<OpNodeT>({left, right}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildTruncatingInt32BinaryOperationNodeForToNumber(
    ToNumberHint hint) {
  static_assert(BinaryOperationIsBitwiseInt32<kOperation>());
  // TODO(v8:7700): Do constant folding.
  ValueNode* left;
  ValueNode* right;
  if (IsRegisterEqualToAccumulator(0)) {
    left = right = GetTruncatedInt32ForToNumber(
        current_interpreter_frame_.get(iterator_.GetRegisterOperand(0)), hint);
  } else {
    left = GetTruncatedInt32ForToNumber(
        current_interpreter_frame_.get(iterator_.GetRegisterOperand(0)), hint);
    right = GetTruncatedInt32ForToNumber(
        current_interpreter_frame_.accumulator(), hint);
  }

  if (ValueNode* result =
          TryFoldInt32BinaryOperation<kOperation>(left, right)) {
    SetAccumulator(result);
    return;
  }
  SetAccumulator(AddNewNode<Int32NodeFor<kOperation>>({left, right}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildInt32BinarySmiOperationNode() {
  // Truncating Int32 nodes treat their input as a signed int32 regardless
  // of whether it's really signed or not, so we allow Uint32 by loading a
  // TruncatedInt32 value.
  static_assert(!BinaryOperationIsBitwiseInt32<kOperation>());
  // TODO(v8:7700): Do constant folding.
  ValueNode* left = GetAccumulatorInt32();
  int32_t constant = iterator_.GetImmediateOperand(0);
  if (base::Optional<int>(constant) == Int32Identity<kOperation>()) {
    // If the constant is the unit of the operation, it already has the right
    // value, so just return.
    return;
  }
  if (ValueNode* result =
          TryFoldInt32BinaryOperation<kOperation>(left, constant)) {
    SetAccumulator(result);
    return;
  }
  ValueNode* right = GetInt32Constant(constant);

  using OpNodeT = Int32NodeFor<kOperation>;

  SetAccumulator(AddNewNode<OpNodeT>({left, right}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildTruncatingInt32BinarySmiOperationNodeForToNumber(
    ToNumberHint hint) {
  static_assert(BinaryOperationIsBitwiseInt32<kOperation>());
  // TODO(v8:7700): Do constant folding.
  ValueNode* left = GetTruncatedInt32ForToNumber(
      current_interpreter_frame_.accumulator(), hint);
  int32_t constant = iterator_.GetImmediateOperand(0);
  if (base::Optional<int>(constant) == Int32Identity<kOperation>()) {
    // If the constant is the unit of the operation, it already has the right
    // value, so use the truncated value (if not just a conversion) and return.
    if (!left->properties().is_conversion()) {
      current_interpreter_frame_.set_accumulator(left);
    }
    return;
  }
  if (ValueNode* result =
          TryFoldInt32BinaryOperation<kOperation>(left, constant)) {
    SetAccumulator(result);
    return;
  }
  ValueNode* right = GetInt32Constant(constant);
  SetAccumulator(AddNewNode<Int32NodeFor<kOperation>>({left, right}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildFloat64BinarySmiOperationNodeForToNumber(
    ToNumberHint hint) {
  // TODO(v8:7700): Do constant folding. Make sure to normalize HoleyFloat64
  // nodes if constant folded.
  ValueNode* left = GetAccumulatorHoleyFloat64ForToNumber(hint);
  double constant = static_cast<double>(iterator_.GetImmediateOperand(0));
  ValueNode* right = GetFloat64Constant(constant);
  SetAccumulator(AddNewNode<Float64NodeFor<kOperation>>({left, right}));
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildFloat64UnaryOperationNodeForToNumber(
    ToNumberHint hint) {
  // TODO(v8:7700): Do constant folding. Make sure to normalize HoleyFloat64
  // nodes if constant folded.
  ValueNode* value = GetAccumulatorHoleyFloat64ForToNumber(hint);
  switch (kOperation) {
    case Operation::kNegate:
      SetAccumulator(AddNewNode<Float64Negate>({value}));
      break;
    case Operation::kIncrement:
      SetAccumulator(AddNewNode<Float64Add>({value, GetFloat64Constant(1)}));
      break;
    case Operation::kDecrement:
      SetAccumulator(
          AddNewNode<Float64Subtract>({value, GetFloat64Constant(1)}));
      break;
    default:
      UNREACHABLE();
  }
}

template <Operation kOperation>
void MaglevGraphBuilder::BuildFloat64BinaryOperationNodeForToNumber(
    ToNumberHint hint) {
  // TODO(v8:7700): Do constant folding. Make sure to normalize HoleyFloat64
  // nodes if constant folded.
  ValueNode* left = LoadRegisterHoleyFloat64ForToNumber(0, hint);
  ValueNode* right = GetAccumulatorHoleyFloat64ForToNumber(hint);
  SetAccumulator(AddNewNode<Float64NodeFor<kOperation>>({left, right}));
}

namespace {
ToNumberHint BinopHintToToNumberHint(BinaryOperationHint hint) {
  switch (hint) {
    case BinaryOperationHint::kSignedSmall:
      return ToNumberHint::kAssumeSmi;
    case BinaryOperationHint::kSignedSmallInputs:
    case BinaryOperationHint::kNumber:
      return ToNumberHint::kAssumeNumber;
    case BinaryOperationHint::kNumberOrOddball:
      return ToNumberHint::kAssumeNumberOrOddball;

    case BinaryOperationHint::kNone:
    case BinaryOperationHint::kString:
    case BinaryOperationHint::kBigInt:
    case BinaryOperationHint::kBigInt64:
    case BinaryOperationHint::kAny:
      UNREACHABLE();
  }
}
}  // namespace

template <Operation kOperation>
void MaglevGraphBuilder::VisitUnaryOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(0);
  BinaryOperationHint feedback_hint = nexus.GetBinaryOperationFeedback();
  switch (feedback_hint) {
    case BinaryOperationHint::kNone:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForBinaryOperation));
    case BinaryOperationHint::kSignedSmall:
    case BinaryOperationHint::kSignedSmallInputs:
    case BinaryOperationHint::kNumber:
    case BinaryOperationHint::kNumberOrOddball: {
      ToNumberHint hint = BinopHintToToNumberHint(feedback_hint);
      if constexpr (BinaryOperationIsBitwiseInt32<kOperation>()) {
        static_assert(kOperation == Operation::kBitwiseNot);
        return BuildTruncatingInt32BitwiseNotForToNumber(hint);
      } else if (feedback_hint == BinaryOperationHint::kSignedSmall) {
        return BuildInt32UnaryOperationNode<kOperation>();
      }
      return BuildFloat64UnaryOperationNodeForToNumber<kOperation>(hint);
      break;
    }
    case BinaryOperationHint::kString:
    case BinaryOperationHint::kBigInt:
    case BinaryOperationHint::kBigInt64:
    case BinaryOperationHint::kAny:
      // Fallback to generic node.
      break;
  }
  BuildGenericUnaryOperationNode<kOperation>();
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitBinaryOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(1);
  BinaryOperationHint feedback_hint = nexus.GetBinaryOperationFeedback();
  switch (feedback_hint) {
    case BinaryOperationHint::kNone:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForBinaryOperation));
    case BinaryOperationHint::kSignedSmall:
    case BinaryOperationHint::kSignedSmallInputs:
    case BinaryOperationHint::kNumber:
    case BinaryOperationHint::kNumberOrOddball: {
      ToNumberHint hint = BinopHintToToNumberHint(feedback_hint);
      if constexpr (BinaryOperationIsBitwiseInt32<kOperation>()) {
        return BuildTruncatingInt32BinaryOperationNodeForToNumber<kOperation>(
            hint);
      } else if (feedback_hint == BinaryOperationHint::kSignedSmall) {
        if constexpr (kOperation == Operation::kExponentiate) {
          // Exponentiate never updates the feedback to be a Smi.
          UNREACHABLE();
        } else {
          return BuildInt32BinaryOperationNode<kOperation>();
        }
      } else {
        return BuildFloat64BinaryOperationNodeForToNumber<kOperation>(hint);
      }
      break;
    }
    case BinaryOperationHint::kString:
      if constexpr (kOperation == Operation::kAdd) {
        ValueNode* left = LoadRegisterTagged(0);
        ValueNode* right = GetAccumulatorTagged();
        BuildCheckString(left);
        BuildCheckString(right);
        SetAccumulator(AddNewNode<StringConcat>({left, right}));
        return;
      }
      break;
    case BinaryOperationHint::kBigInt:
    case BinaryOperationHint::kBigInt64:
    case BinaryOperationHint::kAny:
      // Fallback to generic node.
      break;
  }
  BuildGenericBinaryOperationNode<kOperation>();
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitBinarySmiOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(1);
  BinaryOperationHint feedback_hint = nexus.GetBinaryOperationFeedback();
  switch (feedback_hint) {
    case BinaryOperationHint::kNone:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForBinaryOperation));
    case BinaryOperationHint::kSignedSmall:
    case BinaryOperationHint::kSignedSmallInputs:
    case BinaryOperationHint::kNumber:
    case BinaryOperationHint::kNumberOrOddball: {
      ToNumberHint hint = BinopHintToToNumberHint(feedback_hint);
      if constexpr (BinaryOperationIsBitwiseInt32<kOperation>()) {
        return BuildTruncatingInt32BinarySmiOperationNodeForToNumber<
            kOperation>(hint);
      } else if (feedback_hint == BinaryOperationHint::kSignedSmall) {
        if constexpr (kOperation == Operation::kExponentiate) {
          // Exponentiate never updates the feedback to be a Smi.
          UNREACHABLE();
        } else {
          return BuildInt32BinarySmiOperationNode<kOperation>();
        }
      } else {
        return BuildFloat64BinarySmiOperationNodeForToNumber<kOperation>(hint);
      }
      break;
    }
    case BinaryOperationHint::kString:
    case BinaryOperationHint::kBigInt:
    case BinaryOperationHint::kBigInt64:
    case BinaryOperationHint::kAny:
      // Fallback to generic node.
      break;
  }
  BuildGenericBinarySmiOperationNode<kOperation>();
}

base::Optional<int> MaglevGraphBuilder::TryFindNextBranch(int* inline_level) {
  DisallowGarbageCollection no_gc;
  // Copy the iterator so we can search for the next branch without changing
  // current iterator state.
  interpreter::BytecodeArrayIterator it(iterator_.bytecode_array(),
                                        iterator_.current_offset(), no_gc);

  // Skip the current bytecode.
  it.Advance();

  for (; !it.done(); it.Advance()) {
    // Bail out if there is a merge point before the next branch.
    if (IsOffsetAMergePoint(it.current_offset())) {
      if (v8_flags.trace_maglev_graph_building) {
        std::cout
            << "  ! Bailing out of test->branch fusion because merge point"
            << std::endl;
      }
      return {};
    }
    switch (it.current_bytecode()) {
      case interpreter::Bytecode::kMov:
      case interpreter::Bytecode::kToBoolean:
      case interpreter::Bytecode::kLogicalNot:
      case interpreter::Bytecode::kToBooleanLogicalNot:
        // No register moves, and only affecting the accumulator in a way that
        // can be emulated with swapping branch targets.
        continue;

      case interpreter::Bytecode::kStar: {
        interpreter::Register store_reg = it.GetRegisterOperand(0);
        // If the Star stores the accumulator to a live register, the
        // accumulator boolean value is observable and must be materialized.
        if (store_reg.is_parameter() ||
            GetOutLivenessFor(it.current_offset())
                ->RegisterIsLive(store_reg.index())) {
          if (v8_flags.trace_maglev_graph_building) {
            std::cout << "  ! Bailing out of test->branch fusion because "
                         "accumulator stored to live register"
                      << std::endl;
          }
          return {};
        }
        continue;
      }

#define STAR_CASE(name, ...) case interpreter::Bytecode::k##name:
        SHORT_STAR_BYTECODE_LIST(STAR_CASE)
#undef STAR_CASE
        {
          interpreter::Register store_reg =
              interpreter::Register::FromShortStar(it.current_bytecode());
          if (store_reg.is_parameter() ||
              GetOutLivenessFor(it.current_offset())
                  ->RegisterIsLive(store_reg.index())) {
            if (v8_flags.trace_maglev_graph_building) {
              std::cout << "  ! Bailing out of test->branch fusion because "
                           "accumulator stored to live register"
                        << std::endl;
            }
            return {};
          }
          continue;
        }

      case interpreter::Bytecode::kReturn:
        if (!is_inline()) {
          if (v8_flags.trace_maglev_graph_building) {
            std::cout << "  ! Bailing out of test->branch fusion because no "
                         "branch found"
                      << std::endl;
          }
          return {};
        }
        // The inlined new.target is the root constant iff we are not inlining
        // a constructor. Bail out for constructors.
        // TODO(leszeks): Since we know we are returning a primitive true/false,
        // which will then be replaced with a new.target that ToBooleans to
        // true, we could treat this as an unconditional "true" for branch
        // fusion.
        if (!inlined_new_target_->Is<RootConstant>()) {
          if (v8_flags.trace_maglev_graph_building) {
            std::cout << "  ! Bailing out of test->branch fusion because "
                         "returning from a constructor"
                      << std::endl;
          }
          return {};
        }
        DCHECK_EQ(inlined_new_target_->Cast<RootConstant>()->index(),
                  RootIndex::kUndefinedValue);
        // Check that the return is the last bytecode in the inlined function,
        // without any other returns. Otherwise we would skip a required merge
        // point.
        if (it.next_bytecode() != interpreter::Bytecode::kIllegal ||
            IsOffsetAMergePoint(inline_exit_offset())) {
          if (v8_flags.trace_maglev_graph_building) {
            std::cout
                << "  ! Bailing out of test->branch fusion because returning "
                   "from inlined function with merge points"
                << std::endl;
          }
          return {};
        }
        DCHECK_NOT_NULL(parent_);
        (*inline_level)++;
        return parent_->TryFindNextBranch(inline_level);

      case interpreter::Bytecode::kJumpIfFalse:
      case interpreter::Bytecode::kJumpIfFalseConstant:
      case interpreter::Bytecode::kJumpIfToBooleanFalse:
      case interpreter::Bytecode::kJumpIfToBooleanFalseConstant:
      case interpreter::Bytecode::kJumpIfTrue:
      case interpreter::Bytecode::kJumpIfTrueConstant:
      case interpreter::Bytecode::kJumpIfToBooleanTrue:
      case interpreter::Bytecode::kJumpIfToBooleanTrueConstant:
        return {it.current_offset()};

      default:
        if (v8_flags.trace_maglev_graph_building) {
          std::cout << "  ! Bailing out of test->branch fusion because of "
                       "unsupported bytecode: "
                    << it.current_bytecode() << std::endl;
        }
        return {};
    }
  }

  return {};
}

template <typename BranchControlNodeT, typename... Args>
void MaglevGraphBuilder::BuildFusedBranch(
    int branch_offset, int inline_level, bool init_flip,
    std::initializer_list<ValueNode*> control_inputs, Args&&... args) {
  // Advance past the test.
  iterator_.Advance();

  // Evaluate Movs and LogicalNots between test and jump.
  bool flip = init_flip;
  for (;; iterator_.Advance()) {
    DCHECK_IMPLIES(inline_level == 0,
                   iterator_.current_offset() <= branch_offset);
    UpdateSourceAndBytecodePosition(iterator_.current_offset());
    switch (iterator_.current_bytecode()) {
      case interpreter::Bytecode::kMov: {
        interpreter::Register src = iterator_.GetRegisterOperand(0);
        interpreter::Register dst = iterator_.GetRegisterOperand(1);
        DCHECK_NOT_NULL(current_interpreter_frame_.get(src));
        current_interpreter_frame_.set(dst,
                                       current_interpreter_frame_.get(src));

        continue;
      }
      case interpreter::Bytecode::kToBoolean:
        continue;

      case interpreter::Bytecode::kLogicalNot:
      case interpreter::Bytecode::kToBooleanLogicalNot:
        flip = !flip;
        continue;

      case interpreter::Bytecode::kStar:
#define STAR_CASE(name, ...) case interpreter::Bytecode::k##name:
        SHORT_STAR_BYTECODE_LIST(STAR_CASE)
#undef STAR_CASE
        // We don't need to perform the Star, since the target register is
        // already known to be dead.
        continue;

      case interpreter::Bytecode::kReturn: {
        DCHECK_GT(inline_level, 0);
        terminated_with_fused_branch_ = true;
        parent_->current_block_ = current_block_;
        current_block_ = nullptr;
        parent_->BuildFusedBranch<BranchControlNodeT>(
            branch_offset, inline_level - 1, flip, control_inputs,
            std::forward<Args>(args)...);
        return;
      }
      default:
        // Otherwise, we've reached the jump, so abort the iteration.
        DCHECK_EQ(inline_level, 0);
        DCHECK_EQ(iterator_.current_offset(), branch_offset);
        break;
    }
    break;
  }

  JumpType jump_type;
  switch (iterator_.current_bytecode()) {
    case interpreter::Bytecode::kJumpIfFalse:
    case interpreter::Bytecode::kJumpIfFalseConstant:
    case interpreter::Bytecode::kJumpIfToBooleanFalse:
    case interpreter::Bytecode::kJumpIfToBooleanFalseConstant:
      jump_type = flip ? JumpType::kJumpIfTrue : JumpType::kJumpIfFalse;
      break;
    case interpreter::Bytecode::kJumpIfTrue:
    case interpreter::Bytecode::kJumpIfTrueConstant:
    case interpreter::Bytecode::kJumpIfToBooleanTrue:
    case interpreter::Bytecode::kJumpIfToBooleanTrueConstant:
      jump_type = flip ? JumpType::kJumpIfFalse : JumpType::kJumpIfTrue;
      break;
    default:
      UNREACHABLE();
  }

  int true_offset, false_offset;
  if (jump_type == kJumpIfFalse) {
    true_offset = next_offset();
    false_offset = iterator_.GetJumpTargetOffset();
  } else {
    true_offset = iterator_.GetJumpTargetOffset();
    false_offset = next_offset();
  }

  BasicBlock* block = FinishBlock<BranchControlNodeT>(
      control_inputs, std::forward<Args>(args)..., &jump_targets_[true_offset],
      &jump_targets_[false_offset]);

  SetAccumulatorInBranch(GetBooleanConstant((jump_type == kJumpIfTrue) ^ flip));
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
  SetAccumulatorInBranch(
      GetBooleanConstant((jump_type == kJumpIfFalse) ^ flip));
  StartFallthroughBlock(next_offset(), block);
}

template <typename BranchControlNodeT, bool init_flip, typename... Args>
bool MaglevGraphBuilder::TryBuildBranchFor(
    std::initializer_list<ValueNode*> control_inputs, Args&&... args) {
  int inline_level = 0;
  base::Optional<int> maybe_next_branch_offset =
      TryFindNextBranch(&inline_level);

  // If we didn't find a branch, bail out.
  if (!maybe_next_branch_offset) {
    return false;
  }

  int next_branch_offset = *maybe_next_branch_offset;

  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "  * Fusing test @" << iterator_.current_offset()
              << " and branch @" << next_branch_offset;
    if (inline_level > 0) {
      std::cout << " (in caller " << inline_level << " level(s) out)";
    }
    std::cout << std::endl;
  }

  BuildFusedBranch<BranchControlNodeT>(next_branch_offset, inline_level,
                                       init_flip, control_inputs,
                                       std::forward<Args>(args)...);
  return true;
}

template <Operation kOperation, typename type>
bool OperationValue(type left, type right) {
  switch (kOperation) {
    case Operation::kEqual:
    case Operation::kStrictEqual:
      return left == right;
    case Operation::kLessThan:
      return left < right;
    case Operation::kLessThanOrEqual:
      return left <= right;
    case Operation::kGreaterThan:
      return left > right;
    case Operation::kGreaterThanOrEqual:
      return left >= right;
  }
}

// static
compiler::OptionalHeapObjectRef MaglevGraphBuilder::TryGetConstant(
    compiler::JSHeapBroker* broker, LocalIsolate* isolate, ValueNode* node) {
  if (Constant* c = node->TryCast<Constant>()) {
    return c->object();
  }
  if (RootConstant* c = node->TryCast<RootConstant>()) {
    return MakeRef(broker, isolate->root_handle(c->index())).AsHeapObject();
  }
  return {};
}

compiler::OptionalHeapObjectRef MaglevGraphBuilder::TryGetConstant(
    ValueNode* node, ValueNode** constant_node) {
  if (auto result = TryGetConstant(broker(), local_isolate(), node)) {
    if (constant_node) *constant_node = node;
    return result;
  }
  const NodeInfo* info = known_node_aspects().TryGetInfoFor(node);
  if (info) {
    if (auto c = info->alternative().constant()) {
      if (constant_node) *constant_node = c;
      return TryGetConstant(c);
    }
  }
  return {};
}

template <Operation kOperation>
bool MaglevGraphBuilder::TryReduceCompareEqualAgainstConstant() {
  // First handle strict equal comparison with constant.
  if (kOperation != Operation::kStrictEqual) return false;
  ValueNode* left = LoadRegisterRaw(0);
  ValueNode* right = GetRawAccumulator();

  compiler::OptionalHeapObjectRef maybe_constant = TryGetConstant(left);
  if (!maybe_constant) maybe_constant = TryGetConstant(right);
  if (!maybe_constant) return false;
  InstanceType type = maybe_constant.value().map(broker()).instance_type();

  if (!InstanceTypeChecker::IsReferenceComparable(type)) return false;

  if (left->properties().value_representation() !=
          ValueRepresentation::kTagged ||
      right->properties().value_representation() !=
          ValueRepresentation::kTagged) {
    SetAccumulator(GetBooleanConstant(false));
  } else if (left == right) {
    SetAccumulator(GetBooleanConstant(true));
  } else if (!TryBuildBranchFor<BranchIfReferenceCompare>({left, right},
                                                          kOperation)) {
    SetAccumulator(AddNewNode<TaggedEqual>({left, right}));
  }
  return true;
}

template <Operation kOperation>
void MaglevGraphBuilder::VisitCompareOperation() {
  FeedbackNexus nexus = FeedbackNexusForOperand(1);
  switch (nexus.GetCompareOperationFeedback()) {
    case CompareOperationHint::kNone:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForCompareOperation));

    case CompareOperationHint::kSignedSmall: {
      ValueNode* left = LoadRegisterInt32(0);
      ValueNode* right = GetAccumulatorInt32();
      if (left == right) {
        SetAccumulator(
            GetBooleanConstant(kOperation == Operation::kEqual ||
                               kOperation == Operation::kStrictEqual ||
                               kOperation == Operation::kLessThanOrEqual ||
                               kOperation == Operation::kGreaterThanOrEqual));
        return;
      }
      if (left->Is<Int32Constant>() && right->Is<Int32Constant>()) {
        int left_value = left->Cast<Int32Constant>()->value();
        int right_value = right->Cast<Int32Constant>()->value();
        SetAccumulator(GetBooleanConstant(
            OperationValue<kOperation>(left_value, right_value)));
        return;
      }
      if (TryBuildBranchFor<BranchIfInt32Compare>({left, right}, kOperation)) {
        return;
      }
      SetAccumulator(AddNewNode<Int32NodeFor<kOperation>>({left, right}));
      return;
    }
    case CompareOperationHint::kNumber: {
      // TODO(leszeks): we could support kNumberOrOddball with
      // BranchIfFloat64Compare, but we'd need to special case comparing
      // oddballs with NaN value (e.g. undefined) against themselves.
      ValueNode* left = LoadRegisterFloat64(0);
      ValueNode* right = GetAccumulatorFloat64();
      if (left->Is<Float64Constant>() && right->Is<Float64Constant>()) {
        double left_value = left->Cast<Float64Constant>()->value().get_scalar();
        double right_value =
            right->Cast<Float64Constant>()->value().get_scalar();
        SetAccumulator(GetBooleanConstant(
            OperationValue<kOperation>(left_value, right_value)));
        return;
      }
      if (TryBuildBranchFor<BranchIfFloat64Compare>({left, right},
                                                    kOperation)) {
        return;
      }
      SetAccumulator(AddNewNode<Float64NodeFor<kOperation>>({left, right}));
      return;
    }
    case CompareOperationHint::kInternalizedString: {
      DCHECK(kOperation == Operation::kEqual ||
             kOperation == Operation::kStrictEqual);
      ValueNode *left, *right;
      if (IsRegisterEqualToAccumulator(0)) {
        left = right = GetInternalizedString(iterator_.GetRegisterOperand(0));
        SetAccumulator(GetRootConstant(RootIndex::kTrueValue));
        return;
      }
      left = GetInternalizedString(iterator_.GetRegisterOperand(0));
      right =
          GetInternalizedString(interpreter::Register::virtual_accumulator());
      if (left == right) {
        SetAccumulator(GetBooleanConstant(true));
        return;
      }
      if (TryBuildBranchFor<BranchIfReferenceCompare>({left, right},
                                                      kOperation)) {
        return;
      }
      SetAccumulator(AddNewNode<TaggedEqual>({left, right}));
      return;
    }
    case CompareOperationHint::kSymbol: {
      DCHECK(kOperation == Operation::kEqual ||
             kOperation == Operation::kStrictEqual);

      ValueNode* left = LoadRegisterTagged(0);
      ValueNode* right = GetAccumulatorTagged();
      BuildCheckSymbol(left);
      BuildCheckSymbol(right);
      if (left == right) {
        SetAccumulator(GetBooleanConstant(true));
        return;
      }
      if (TryBuildBranchFor<BranchIfReferenceCompare>({left, right},
                                                      kOperation)) {
        return;
      }
      SetAccumulator(AddNewNode<TaggedEqual>({left, right}));
      return;
    }
    case CompareOperationHint::kString: {
      if (TryReduceCompareEqualAgainstConstant<kOperation>()) return;

      ValueNode* left = LoadRegisterTagged(0);
      ValueNode* right = GetAccumulatorTagged();
      BuildCheckString(left);
      BuildCheckString(right);

      ValueNode* result;
      if (left == right) {
        SetAccumulator(
            GetBooleanConstant(kOperation == Operation::kEqual ||
                               kOperation == Operation::kStrictEqual ||
                               kOperation == Operation::kLessThanOrEqual ||
                               kOperation == Operation::kGreaterThanOrEqual));
        return;
      }
      switch (kOperation) {
        case Operation::kEqual:
        case Operation::kStrictEqual:
          result = AddNewNode<StringEqual>({left, right});
          break;
        case Operation::kLessThan:
          result = BuildCallBuiltin<Builtin::kStringLessThan>({left, right});
          break;
        case Operation::kLessThanOrEqual:
          result =
              BuildCallBuiltin<Builtin::kStringLessThanOrEqual>({left, right});
          break;
        case Operation::kGreaterThan:
          result = BuildCallBuiltin<Builtin::kStringGreaterThan>({left, right});
          break;
        case Operation::kGreaterThanOrEqual:
          result = BuildCallBuiltin<Builtin::kStringGreaterThanOrEqual>(
              {left, right});
          break;
      }

      SetAccumulator(result);
      return;
    }
    case CompareOperationHint::kAny:
    case CompareOperationHint::kBigInt64:
    case CompareOperationHint::kBigInt:
    case CompareOperationHint::kNumberOrBoolean:
    case CompareOperationHint::kNumberOrOddball:
    case CompareOperationHint::kReceiverOrNullOrUndefined:
      if (TryReduceCompareEqualAgainstConstant<kOperation>()) return;
      break;
    case CompareOperationHint::kReceiver: {
      if (TryReduceCompareEqualAgainstConstant<kOperation>()) return;
      DCHECK(kOperation == Operation::kEqual ||
             kOperation == Operation::kStrictEqual);

      ValueNode* left = LoadRegisterTagged(0);
      ValueNode* right = GetAccumulatorTagged();
      BuildCheckJSReceiver(left);
      BuildCheckJSReceiver(right);
      if (left == right) {
        SetAccumulator(GetBooleanConstant(true));
        return;
      }
      if (TryBuildBranchFor<BranchIfReferenceCompare>({left, right},
                                                      kOperation)) {
        return;
      }
      SetAccumulator(AddNewNode<TaggedEqual>({left, right}));
      return;
    }
  }

  BuildGenericBinaryOperationNode<kOperation>();
}

void MaglevGraphBuilder::VisitLdar() {
  MoveNodeBetweenRegisters(iterator_.GetRegisterOperand(0),
                           interpreter::Register::virtual_accumulator());
}

void MaglevGraphBuilder::VisitLdaZero() { SetAccumulator(GetSmiConstant(0)); }
void MaglevGraphBuilder::VisitLdaSmi() {
  int constant = iterator_.GetImmediateOperand(0);
  SetAccumulator(GetSmiConstant(constant));
}
void MaglevGraphBuilder::VisitLdaUndefined() {
  SetAccumulator(GetRootConstant(RootIndex::kUndefinedValue));
}
void MaglevGraphBuilder::VisitLdaNull() {
  SetAccumulator(GetRootConstant(RootIndex::kNullValue));
}
void MaglevGraphBuilder::VisitLdaTheHole() {
  SetAccumulator(GetRootConstant(RootIndex::kTheHoleValue));
}
void MaglevGraphBuilder::VisitLdaTrue() {
  SetAccumulator(GetRootConstant(RootIndex::kTrueValue));
}
void MaglevGraphBuilder::VisitLdaFalse() {
  SetAccumulator(GetRootConstant(RootIndex::kFalseValue));
}
void MaglevGraphBuilder::VisitLdaConstant() {
  SetAccumulator(GetConstant(GetRefOperand<HeapObject>(0)));
}

bool MaglevGraphBuilder::TrySpecializeLoadContextSlotToFunctionContext(
    ValueNode** context, size_t* depth, int slot_index,
    ContextSlotMutability slot_mutability) {
  DCHECK(compilation_unit_->info()->specialize_to_function_context());

  size_t new_depth = *depth;
  compiler::OptionalContextRef maybe_context_ref =
      FunctionContextSpecialization::TryToRef(compilation_unit_, *context,
                                              &new_depth);
  if (!maybe_context_ref.has_value()) return false;

  compiler::ContextRef context_ref = maybe_context_ref.value();
  if (slot_mutability == kMutable || new_depth != 0) {
    *depth = new_depth;
    *context = GetConstant(context_ref);
    return false;
  }

  compiler::OptionalObjectRef maybe_slot_value =
      context_ref.get(broker(), slot_index);
  if (!maybe_slot_value.has_value()) {
    *depth = new_depth;
    *context = GetConstant(context_ref);
    return false;
  }

  compiler::ObjectRef slot_value = maybe_slot_value.value();
  if (slot_value.IsHeapObject()) {
    // Even though the context slot is immutable, the context might have escaped
    // before the function to which it belongs has initialized the slot.  We
    // must be conservative and check if the value in the slot is currently the
    // hole or undefined. Only if it is neither of these, can we be sure that it
    // won't change anymore.
    //
    // See also: JSContextSpecialization::ReduceJSLoadContext.
    compiler::OddballType oddball_type =
        slot_value.AsHeapObject().map(broker()).oddball_type(broker());
    if (oddball_type == compiler::OddballType::kUndefined ||
        slot_value.IsTheHole()) {
      *depth = new_depth;
      *context = GetConstant(context_ref);
      return false;
    }
  }

  // Fold the load of the immutable slot.

  SetAccumulator(GetConstant(slot_value));
  return true;
}

ValueNode* MaglevGraphBuilder::LoadAndCacheContextSlot(
    ValueNode* context, int offset, ContextSlotMutability slot_mutability) {
  ValueNode*& cached_value =
      slot_mutability == ContextSlotMutability::kMutable
          ? known_node_aspects().loaded_context_slots[{context, offset}]
          : known_node_aspects().loaded_context_constants[{context, offset}];
  if (cached_value) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  * Reusing cached context slot "
                << PrintNodeLabel(graph_labeller(), context) << "[" << offset
                << "]: " << PrintNode(graph_labeller(), cached_value)
                << std::endl;
    }
    return cached_value;
  }
  return cached_value = AddNewNode<LoadTaggedField>({context}, offset);
}

void MaglevGraphBuilder::StoreAndCacheContextSlot(ValueNode* context,
                                                  int offset,
                                                  ValueNode* value) {
  DCHECK_EQ(
      known_node_aspects().loaded_context_constants.count({context, offset}),
      0);
  BuildStoreTaggedField(context, GetTaggedValue(value), offset);

  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "  * Recording context slot store "
              << PrintNodeLabel(graph_labeller(), context) << "[" << offset
              << "]: " << PrintNode(graph_labeller(), value) << std::endl;
  }
  known_node_aspects().loaded_context_slots[{context, offset}] = value;
}

void MaglevGraphBuilder::BuildLoadContextSlot(
    ValueNode* context, size_t depth, int slot_index,
    ContextSlotMutability slot_mutability) {
  MinimizeContextChainDepth(&context, &depth);

  if (compilation_unit_->info()->specialize_to_function_context() &&
      TrySpecializeLoadContextSlotToFunctionContext(
          &context, &depth, slot_index, slot_mutability)) {
    return;  // Our work here is done.
  }

  for (size_t i = 0; i < depth; ++i) {
    context = LoadAndCacheContextSlot(
        context, Context::OffsetOfElementAt(Context::PREVIOUS_INDEX),
        kImmutable);
  }

  // Always load the slot here as if it were mutable. Immutable slots have a
  // narrow range of mutability if the context escapes before the slot is
  // initialized, so we can't safely assume that the load can be cached in case
  // it's a load before initialization (e.g. var a = a + 42).
  current_interpreter_frame_.set_accumulator(LoadAndCacheContextSlot(
      context, Context::OffsetOfElementAt(slot_index), kMutable));
}

void MaglevGraphBuilder::BuildStoreContextSlot(ValueNode* context, size_t depth,
                                               int slot_index,
                                               ValueNode* value) {
  MinimizeContextChainDepth(&context, &depth);

  if (compilation_unit_->info()->specialize_to_function_context()) {
    compiler::OptionalContextRef maybe_ref =
        FunctionContextSpecialization::TryToRef(compilation_unit_, context,
                                                &depth);
    if (maybe_ref.has_value()) {
      context = GetConstant(maybe_ref.value());
    }
  }

  for (size_t i = 0; i < depth; ++i) {
    context = LoadAndCacheContextSlot(
        context, Context::OffsetOfElementAt(Context::PREVIOUS_INDEX),
        kImmutable);
  }

  StoreAndCacheContextSlot(context, Context::OffsetOfElementAt(slot_index),
                           value);
}

void MaglevGraphBuilder::VisitLdaContextSlot() {
  ValueNode* context = LoadRegisterTagged(0);
  int slot_index = iterator_.GetIndexOperand(1);
  size_t depth = iterator_.GetUnsignedImmediateOperand(2);
  BuildLoadContextSlot(context, depth, slot_index, kMutable);
}
void MaglevGraphBuilder::VisitLdaImmutableContextSlot() {
  ValueNode* context = LoadRegisterTagged(0);
  int slot_index = iterator_.GetIndexOperand(1);
  size_t depth = iterator_.GetUnsignedImmediateOperand(2);
  BuildLoadContextSlot(context, depth, slot_index, kImmutable);
}
void MaglevGraphBuilder::VisitLdaCurrentContextSlot() {
  ValueNode* context = GetContext();
  int slot_index = iterator_.GetIndexOperand(0);
  BuildLoadContextSlot(context, 0, slot_index, kMutable);
}
void MaglevGraphBuilder::VisitLdaImmutableCurrentContextSlot() {
  ValueNode* context = GetContext();
  int slot_index = iterator_.GetIndexOperand(0);
  BuildLoadContextSlot(context, 0, slot_index, kImmutable);
}

void MaglevGraphBuilder::VisitStaContextSlot() {
  ValueNode* context = LoadRegisterTagged(0);
  int slot_index = iterator_.GetIndexOperand(1);
  size_t depth = iterator_.GetUnsignedImmediateOperand(2);
  BuildStoreContextSlot(context, depth, slot_index, GetRawAccumulator());
}
void MaglevGraphBuilder::VisitStaCurrentContextSlot() {
  ValueNode* context = GetContext();
  int slot_index = iterator_.GetIndexOperand(0);
  BuildStoreContextSlot(context, 0, slot_index, GetRawAccumulator());
}

void MaglevGraphBuilder::VisitStar() {
  MoveNodeBetweenRegisters(interpreter::Register::virtual_accumulator(),
                           iterator_.GetRegisterOperand(0));
}
#define SHORT_STAR_VISITOR(Name, ...)                                          \
  void MaglevGraphBuilder::Visit##Name() {                                     \
    MoveNodeBetweenRegisters(                                                  \
        interpreter::Register::virtual_accumulator(),                          \
        interpreter::Register::FromShortStar(interpreter::Bytecode::k##Name)); \
  }
SHORT_STAR_BYTECODE_LIST(SHORT_STAR_VISITOR)
#undef SHORT_STAR_VISITOR

void MaglevGraphBuilder::VisitMov() {
  MoveNodeBetweenRegisters(iterator_.GetRegisterOperand(0),
                           iterator_.GetRegisterOperand(1));
}

void MaglevGraphBuilder::VisitPushContext() {
  MoveNodeBetweenRegisters(interpreter::Register::current_context(),
                           iterator_.GetRegisterOperand(0));
  SetContext(GetAccumulatorTagged());
}

void MaglevGraphBuilder::VisitPopContext() {
  SetContext(LoadRegisterTagged(0));
}

void MaglevGraphBuilder::VisitTestReferenceEqual() {
  ValueNode* lhs = LoadRegisterTagged(0);
  ValueNode* rhs = GetAccumulatorTagged();
  if (lhs == rhs) {
    SetAccumulator(GetRootConstant(RootIndex::kTrueValue));
    return;
  }
  if (TryBuildBranchFor<BranchIfReferenceCompare>({lhs, rhs},
                                                  Operation::kStrictEqual)) {
    return;
  }
  SetAccumulator(AddNewNode<TaggedEqual>({lhs, rhs}));
}

void MaglevGraphBuilder::VisitTestUndetectable() {
  ValueNode* value = GetAccumulatorTagged();
  if (compiler::OptionalHeapObjectRef maybe_constant = TryGetConstant(value)) {
    if (maybe_constant.value().map(broker()).is_undetectable()) {
      SetAccumulator(GetRootConstant(RootIndex::kTrueValue));
    } else {
      SetAccumulator(GetRootConstant(RootIndex::kFalseValue));
    }
    return;
  }

  NodeType old_type;
  if (CheckType(value, NodeType::kSmi, &old_type)) {
    SetAccumulator(GetRootConstant(RootIndex::kFalseValue));
    return;
  }

  enum CheckType type = GetCheckType(old_type);
  if (TryBuildBranchFor<BranchIfUndetectable>({value}, type)) return;
  SetAccumulator(AddNewNode<TestUndetectable>({value}, type));
}

void MaglevGraphBuilder::VisitTestNull() {
  ValueNode* value = GetAccumulatorTagged();
  if (IsConstantNode(value->opcode())) {
    SetAccumulator(GetBooleanConstant(IsNullValue(value)));
    return;
  }
  if (TryBuildBranchFor<BranchIfRootConstant>({value}, RootIndex::kNullValue)) {
    return;
  }
  ValueNode* null_constant = GetRootConstant(RootIndex::kNullValue);
  SetAccumulator(AddNewNode<TaggedEqual>({value, null_constant}));
}

void MaglevGraphBuilder::VisitTestUndefined() {
  ValueNode* value = GetAccumulatorTagged();
  if (IsConstantNode(value->opcode())) {
    SetAccumulator(GetBooleanConstant(IsUndefinedValue(value)));
    return;
  }
  if (TryBuildBranchFor<BranchIfRootConstant>({value},
                                              RootIndex::kUndefinedValue)) {
    return;
  }
  ValueNode* undefined_constant = GetRootConstant(RootIndex::kUndefinedValue);
  SetAccumulator(AddNewNode<TaggedEqual>({value, undefined_constant}));
}

void MaglevGraphBuilder::VisitTestTypeOf() {
  using LiteralFlag = interpreter::TestTypeOfFlags::LiteralFlag;
  // TODO(v8:7700): Add a branch version of TestTypeOf that does not need to
  // materialise the boolean value.
  LiteralFlag literal =
      interpreter::TestTypeOfFlags::Decode(GetFlag8Operand(0));
  if (literal == LiteralFlag::kOther) {
    SetAccumulator(GetRootConstant(RootIndex::kFalseValue));
    return;
  }
  ValueNode* value = GetAccumulatorTagged();
  if (TryBuildBranchFor<BranchIfTypeOf>({value}, literal)) {
    return;
  }
  SetAccumulator(AddNewNode<TestTypeOf>({value}, literal));
}

ReduceResult MaglevGraphBuilder::TryBuildScriptContextStore(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  DCHECK(global_access_feedback.IsScriptContextSlot());
  if (global_access_feedback.immutable()) {
    return ReduceResult::Fail();
  }
  auto script_context = GetConstant(global_access_feedback.script_context());
  int offset = Context::OffsetOfElementAt(global_access_feedback.slot_index());
  StoreAndCacheContextSlot(script_context, offset, GetRawAccumulator());
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::TryBuildPropertyCellStore(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  DCHECK(global_access_feedback.IsPropertyCell());

  compiler::PropertyCellRef property_cell =
      global_access_feedback.property_cell();
  if (!property_cell.Cache(broker())) return ReduceResult::Fail();

  compiler::ObjectRef property_cell_value = property_cell.value(broker());
  if (property_cell_value.IsTheHole()) {
    // The property cell is no longer valid.
    return EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess);
  }

  PropertyDetails property_details = property_cell.property_details();
  DCHECK_EQ(PropertyKind::kData, property_details.kind());

  if (property_details.IsReadOnly()) {
    // Don't even bother trying to lower stores to read-only data
    // properties.
    // TODO(neis): We could generate code that checks if the new value
    // equals the old one and then does nothing or deopts, respectively.
    return ReduceResult::Fail();
  }

  switch (property_details.cell_type()) {
    case PropertyCellType::kUndefined:
      return ReduceResult::Fail();
    case PropertyCellType::kConstant: {
      // TODO(victorgomes): Support non-internalized string.
      if (property_cell_value.IsString() &&
          !property_cell_value.IsInternalizedString()) {
        return ReduceResult::Fail();
      }
      // Record a code dependency on the cell, and just deoptimize if the new
      // value doesn't match the previous value stored inside the cell.
      broker()->dependencies()->DependOnGlobalProperty(property_cell);
      ValueNode* value = GetAccumulatorTagged();
      return BuildCheckValue(value, property_cell_value);
    }
    case PropertyCellType::kConstantType: {
      // We rely on stability further below.
      if (property_cell_value.IsHeapObject() &&
          !property_cell_value.AsHeapObject().map(broker()).is_stable()) {
        return ReduceResult::Fail();
      }
      // Record a code dependency on the cell, and just deoptimize if the new
      // value's type doesn't match the type of the previous value in the cell.
      broker()->dependencies()->DependOnGlobalProperty(property_cell);
      ValueNode* value;
      if (property_cell_value.IsHeapObject()) {
        value = GetAccumulatorTagged();
        compiler::MapRef property_cell_value_map =
            property_cell_value.AsHeapObject().map(broker());
        broker()->dependencies()->DependOnStableMap(property_cell_value_map);
        BuildCheckHeapObject(value);
        RETURN_IF_ABORT(
            BuildCheckMaps(value, base::VectorOf({property_cell_value_map})));
      } else {
        value = GetAccumulatorSmi();
      }
      ValueNode* property_cell_node = GetConstant(property_cell.AsHeapObject());
      BuildStoreTaggedField(property_cell_node, value,
                            PropertyCell::kValueOffset);
      break;
    }
    case PropertyCellType::kMutable: {
      // Record a code dependency on the cell, and just deoptimize if the
      // property ever becomes read-only.
      broker()->dependencies()->DependOnGlobalProperty(property_cell);
      ValueNode* property_cell_node = GetConstant(property_cell.AsHeapObject());
      ValueNode* value = GetAccumulatorTagged();
      BuildStoreTaggedField(property_cell_node, value,
                            PropertyCell::kValueOffset);
      break;
    }
    case PropertyCellType::kInTransition:
      UNREACHABLE();
  }
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::TryBuildScriptContextConstantLoad(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  DCHECK(global_access_feedback.IsScriptContextSlot());
  if (!global_access_feedback.immutable()) return ReduceResult::Fail();
  compiler::OptionalObjectRef maybe_slot_value =
      global_access_feedback.script_context().get(
          broker(), global_access_feedback.slot_index());
  if (!maybe_slot_value) return ReduceResult::Fail();
  return GetConstant(maybe_slot_value.value());
}

ReduceResult MaglevGraphBuilder::TryBuildScriptContextLoad(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  DCHECK(global_access_feedback.IsScriptContextSlot());
  RETURN_IF_DONE(TryBuildScriptContextConstantLoad(global_access_feedback));
  auto script_context = GetConstant(global_access_feedback.script_context());
  int offset = Context::OffsetOfElementAt(global_access_feedback.slot_index());
  return LoadAndCacheContextSlot(
      script_context, offset,
      global_access_feedback.immutable() ? kImmutable : kMutable);
}

ReduceResult MaglevGraphBuilder::TryBuildPropertyCellLoad(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  // TODO(leszeks): A bunch of this is copied from
  // js-native-context-specialization.cc -- I wonder if we can unify it
  // somehow.
  DCHECK(global_access_feedback.IsPropertyCell());

  compiler::PropertyCellRef property_cell =
      global_access_feedback.property_cell();
  if (!property_cell.Cache(broker())) return ReduceResult::Fail();

  compiler::ObjectRef property_cell_value = property_cell.value(broker());
  if (property_cell_value.IsTheHole()) {
    // The property cell is no longer valid.
    return EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess);
  }

  PropertyDetails property_details = property_cell.property_details();
  PropertyCellType property_cell_type = property_details.cell_type();
  DCHECK_EQ(PropertyKind::kData, property_details.kind());

  if (!property_details.IsConfigurable() && property_details.IsReadOnly()) {
    return GetConstant(property_cell_value);
  }

  // Record a code dependency on the cell if we can benefit from the
  // additional feedback, or the global property is configurable (i.e.
  // can be deleted or reconfigured to an accessor property).
  if (property_cell_type != PropertyCellType::kMutable ||
      property_details.IsConfigurable()) {
    broker()->dependencies()->DependOnGlobalProperty(property_cell);
  }

  // Load from constant/undefined global property can be constant-folded.
  if (property_cell_type == PropertyCellType::kConstant ||
      property_cell_type == PropertyCellType::kUndefined) {
    return GetConstant(property_cell_value);
  }

  ValueNode* property_cell_node = GetConstant(property_cell.AsHeapObject());
  return AddNewNode<LoadTaggedField>({property_cell_node},
                                     PropertyCell::kValueOffset);
}

ReduceResult MaglevGraphBuilder::TryBuildGlobalStore(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  if (global_access_feedback.IsScriptContextSlot()) {
    return TryBuildScriptContextStore(global_access_feedback);
  } else if (global_access_feedback.IsPropertyCell()) {
    return TryBuildPropertyCellStore(global_access_feedback);
  } else {
    DCHECK(global_access_feedback.IsMegamorphic());
    return ReduceResult::Fail();
  }
}

ReduceResult MaglevGraphBuilder::TryBuildGlobalLoad(
    const compiler::GlobalAccessFeedback& global_access_feedback) {
  if (global_access_feedback.IsScriptContextSlot()) {
    return TryBuildScriptContextLoad(global_access_feedback);
  } else if (global_access_feedback.IsPropertyCell()) {
    return TryBuildPropertyCellLoad(global_access_feedback);
  } else {
    DCHECK(global_access_feedback.IsMegamorphic());
    return ReduceResult::Fail();
  }
}

void MaglevGraphBuilder::VisitLdaGlobal() {
  // LdaGlobal <name_index> <slot>

  static const int kNameOperandIndex = 0;
  static const int kSlotOperandIndex = 1;

  compiler::NameRef name = GetRefOperand<Name>(kNameOperandIndex);
  FeedbackSlot slot = GetSlotOperand(kSlotOperandIndex);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  BuildLoadGlobal(name, feedback_source, TypeofMode::kNotInside);
}

void MaglevGraphBuilder::VisitLdaGlobalInsideTypeof() {
  // LdaGlobalInsideTypeof <name_index> <slot>

  static const int kNameOperandIndex = 0;
  static const int kSlotOperandIndex = 1;

  compiler::NameRef name = GetRefOperand<Name>(kNameOperandIndex);
  FeedbackSlot slot = GetSlotOperand(kSlotOperandIndex);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  BuildLoadGlobal(name, feedback_source, TypeofMode::kInside);
}

void MaglevGraphBuilder::VisitStaGlobal() {
  // StaGlobal <name_index> <slot>
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& access_feedback =
      broker()->GetFeedbackForGlobalAccess(feedback_source);

  if (access_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForGenericGlobalAccess));
  }

  const compiler::GlobalAccessFeedback& global_access_feedback =
      access_feedback.AsGlobalAccess();
  RETURN_VOID_IF_DONE(TryBuildGlobalStore(global_access_feedback));

  ValueNode* value = GetAccumulatorTagged();
  compiler::NameRef name = GetRefOperand<Name>(0);
  ValueNode* context = GetContext();
  AddNewNode<StoreGlobal>({context, value}, name, feedback_source);
}

void MaglevGraphBuilder::VisitLdaLookupSlot() {
  // LdaLookupSlot <name_index>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  SetAccumulator(BuildCallRuntime(Runtime::kLoadLookupSlot, {name}));
}

void MaglevGraphBuilder::VisitLdaLookupContextSlot() {
  // LdaLookupContextSlot <name_index> <feedback_slot> <depth>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  ValueNode* slot = GetSmiConstant(iterator_.GetIndexOperand(1));
  ValueNode* depth = GetSmiConstant(iterator_.GetUnsignedImmediateOperand(2));
  SetAccumulator(
      BuildCallBuiltin<Builtin::kLookupContextTrampoline>({name, depth, slot}));
}

void MaglevGraphBuilder::VisitLdaLookupGlobalSlot() {
  // LdaLookupGlobalSlot <name_index> <feedback_slot> <depth>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  ValueNode* slot = GetSmiConstant(iterator_.GetIndexOperand(1));
  ValueNode* depth = GetSmiConstant(iterator_.GetUnsignedImmediateOperand(2));
  ValueNode* result;
  if (parent_) {
    ValueNode* vector = GetConstant(feedback());
    result =
        BuildCallBuiltin<Builtin::kLookupGlobalIC>({name, depth, slot, vector});
  } else {
    result = BuildCallBuiltin<Builtin::kLookupGlobalICTrampoline>(
        {name, depth, slot});
  }
  SetAccumulator(result);
}

void MaglevGraphBuilder::VisitLdaLookupSlotInsideTypeof() {
  // LdaLookupSlotInsideTypeof <name_index>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  SetAccumulator(
      BuildCallRuntime(Runtime::kLoadLookupSlotInsideTypeof, {name}));
}

void MaglevGraphBuilder::VisitLdaLookupContextSlotInsideTypeof() {
  // LdaLookupContextSlotInsideTypeof <name_index> <context_slot> <depth>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  ValueNode* slot = GetSmiConstant(iterator_.GetIndexOperand(1));
  ValueNode* depth = GetSmiConstant(iterator_.GetUnsignedImmediateOperand(2));
  SetAccumulator(
      BuildCallBuiltin<Builtin::kLookupContextInsideTypeofTrampoline>(
          {name, depth, slot}));
}

void MaglevGraphBuilder::VisitLdaLookupGlobalSlotInsideTypeof() {
  // LdaLookupGlobalSlotInsideTypeof <name_index> <context_slot> <depth>
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  ValueNode* slot = GetSmiConstant(iterator_.GetIndexOperand(1));
  ValueNode* depth = GetSmiConstant(iterator_.GetUnsignedImmediateOperand(2));
  ValueNode* result;
  if (parent_) {
    ValueNode* vector = GetConstant(feedback());
    result = BuildCallBuiltin<Builtin::kLookupGlobalICInsideTypeof>(
        {name, depth, slot, vector});
  } else {
    result = BuildCallBuiltin<Builtin::kLookupGlobalICInsideTypeofTrampoline>(
        {name, depth, slot});
  }
  SetAccumulator(result);
}

namespace {
Runtime::FunctionId StaLookupSlotFunction(uint8_t sta_lookup_slot_flags) {
  using Flags = interpreter::StoreLookupSlotFlags;
  switch (Flags::GetLanguageMode(sta_lookup_slot_flags)) {
    case LanguageMode::kStrict:
      return Runtime::kStoreLookupSlot_Strict;
    case LanguageMode::kSloppy:
      if (Flags::IsLookupHoistingMode(sta_lookup_slot_flags)) {
        return Runtime::kStoreLookupSlot_SloppyHoisting;
      } else {
        return Runtime::kStoreLookupSlot_Sloppy;
      }
  }
}
}  // namespace

void MaglevGraphBuilder::VisitStaLookupSlot() {
  // StaLookupSlot <name_index> <flags>
  ValueNode* value = GetAccumulatorTagged();
  ValueNode* name = GetConstant(GetRefOperand<Name>(0));
  uint32_t flags = GetFlag8Operand(1);
  SetAccumulator(BuildCallRuntime(StaLookupSlotFunction(flags), {name, value}));
}

NodeType StaticTypeForNode(compiler::JSHeapBroker* broker,
                           LocalIsolate* isolate, ValueNode* node) {
  switch (node->properties().value_representation()) {
    case ValueRepresentation::kInt32:
    case ValueRepresentation::kUint32:
    case ValueRepresentation::kFloat64:
      return NodeType::kNumber;
    case ValueRepresentation::kHoleyFloat64:
      return NodeType::kNumberOrOddball;
    case ValueRepresentation::kWord64:
      UNREACHABLE();
    case ValueRepresentation::kTagged:
      break;
  }
  switch (node->opcode()) {
    case Opcode::kPhi:
      return node->Cast<Phi>()->type();
    case Opcode::kCheckedSmiTagInt32:
    case Opcode::kCheckedSmiTagUint32:
    case Opcode::kCheckedSmiTagFloat64:
    case Opcode::kUnsafeSmiTag:
    case Opcode::kSmiConstant:
      return NodeType::kSmi;
    case Opcode::kInt32ToNumber:
    case Opcode::kUint32ToNumber:
    case Opcode::kFloat64ToTagged:
      return NodeType::kNumber;
    case Opcode::kHoleyFloat64ToTagged:
      return NodeType::kNumberOrOddball;
    case Opcode::kAllocateRaw:
    case Opcode::kFoldedAllocation:
      return NodeType::kAnyHeapObject;
    case Opcode::kRootConstant: {
      RootConstant* constant = node->Cast<RootConstant>();
      switch (constant->index()) {
        case RootIndex::kTrueValue:
        case RootIndex::kFalseValue:
          return NodeType::kBoolean;
        case RootIndex::kUndefinedValue:
        case RootIndex::kNullValue:
          return NodeType::kOddball;
        default:
          break;
      }
      V8_FALLTHROUGH;
    }
    case Opcode::kConstant: {
      compiler::HeapObjectRef ref =
          MaglevGraphBuilder::TryGetConstant(broker, isolate, node).value();
      return StaticTypeForConstant(broker, ref);
    }
    case Opcode::kLoadPolymorphicTaggedField: {
      Representation field_representation =
          node->Cast<LoadPolymorphicTaggedField>()->field_representation();
      switch (field_representation.kind()) {
        case Representation::kSmi:
          return NodeType::kSmi;
        case Representation::kHeapObject:
          return NodeType::kAnyHeapObject;
        default:
          return NodeType::kUnknown;
      }
    }
    case Opcode::kToNumberOrNumeric:
      if (node->Cast<ToNumberOrNumeric>()->mode() ==
          Object::Conversion::kToNumber) {
        return NodeType::kNumber;
      }
      // TODO(verwaest): Check what we need here.
      return NodeType::kUnknown;
    case Opcode::kToString:
    case Opcode::kNumberToString:
    case Opcode::kStringConcat:
      return NodeType::kString;
    case Opcode::kCheckedInternalizedString:
      return NodeType::kInternalizedString;
    case Opcode::kToObject:
      return NodeType::kJSReceiver;
    case Opcode::kToName:
      return NodeType::kName;
    case Opcode::kFastCreateClosure:
    case Opcode::kCreateClosure:
      return NodeType::kCallable;
    case Opcode::kFloat64Equal:
    case Opcode::kFloat64GreaterThan:
    case Opcode::kFloat64GreaterThanOrEqual:
    case Opcode::kFloat64LessThan:
    case Opcode::kFloat64LessThanOrEqual:
    case Opcode::kFloat64StrictEqual:
    case Opcode::kInt32Equal:
    case Opcode::kInt32GreaterThan:
    case Opcode::kInt32GreaterThanOrEqual:
    case Opcode::kInt32LessThan:
    case Opcode::kInt32LessThanOrEqual:
    case Opcode::kInt32StrictEqual:
    case Opcode::kGenericEqual:
    case Opcode::kGenericStrictEqual:
    case Opcode::kGenericLessThan:
    case Opcode::kGenericLessThanOrEqual:
    case Opcode::kGenericGreaterThan:
    case Opcode::kGenericGreaterThanOrEqual:
    case Opcode::kLogicalNot:
    case Opcode::kStringEqual:
    case Opcode::kTaggedEqual:
    case Opcode::kTaggedNotEqual:
    case Opcode::kTestInstanceOf:
    case Opcode::kTestTypeOf:
    case Opcode::kTestUndetectable:
    case Opcode::kToBoolean:
    case Opcode::kToBooleanLogicalNot:
      return NodeType::kBoolean;
    default:
      return NodeType::kUnknown;
  }
}

bool MaglevGraphBuilder::CheckStaticType(ValueNode* node, NodeType type,
                                         NodeType* old_type) {
  NodeType static_type = StaticTypeForNode(broker(), local_isolate(), node);
  if (old_type) *old_type = static_type;
  return NodeTypeIs(static_type, type);
}

bool MaglevGraphBuilder::EnsureType(ValueNode* node, NodeType type,
                                    NodeType* old_type) {
  if (CheckStaticType(node, type, old_type)) return true;
  NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(node);
  if (old_type) *old_type = known_info->type();
  if (NodeTypeIs(known_info->type(), type)) return true;
  known_info->CombineType(type);
  return false;
}

template <typename Function>
bool MaglevGraphBuilder::EnsureType(ValueNode* node, NodeType type,
                                    Function ensure_new_type) {
  if (CheckStaticType(node, type)) return true;
  NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(node);
  if (NodeTypeIs(known_info->type(), type)) return true;
  ensure_new_type(known_info->type());
  known_info->CombineType(type);
  return false;
}

void MaglevGraphBuilder::SetKnownValue(ValueNode* node,
                                       compiler::ObjectRef ref) {
  DCHECK(!node->Is<Constant>());
  DCHECK(!node->Is<RootConstant>());
  NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(node);
  known_info->CombineType(StaticTypeForConstant(broker(), ref));
  known_info->alternative().set_constant(GetConstant(ref));
}

bool MaglevGraphBuilder::CheckType(ValueNode* node, NodeType type,
                                   NodeType* old_type) {
  if (CheckStaticType(node, type, old_type)) return true;
  auto it = known_node_aspects().FindInfo(node);
  if (!known_node_aspects().IsValid(it)) return false;
  if (old_type) *old_type = it->second.type();
  return NodeTypeIs(it->second.type(), type);
}

ValueNode* MaglevGraphBuilder::BuildSmiUntag(ValueNode* node) {
  if (EnsureType(node, NodeType::kSmi)) {
    return AddNewNode<UnsafeSmiUntag>({node});
  } else {
    return AddNewNode<CheckedSmiUntag>({node});
  }
}

namespace {
NodeType TaggedToFloat64ConversionTypeToNodeType(
    TaggedToFloat64ConversionType conversion_type) {
  switch (conversion_type) {
    case TaggedToFloat64ConversionType::kOnlyNumber:
      return NodeType::kNumber;
    case TaggedToFloat64ConversionType::kNumberOrOddball:
      return NodeType::kNumberOrOddball;
  }
}
}  // namespace

ValueNode* MaglevGraphBuilder::BuildNumberOrOddballToFloat64(
    ValueNode* node, TaggedToFloat64ConversionType conversion_type) {
  NodeType old_type;
  if (EnsureType(node, TaggedToFloat64ConversionTypeToNodeType(conversion_type),
                 &old_type)) {
    if (old_type == NodeType::kSmi) {
      ValueNode* untagged_smi = AddNewNode<UnsafeSmiUntag>({node});
      return AddNewNode<ChangeInt32ToFloat64>({untagged_smi});
    }
    return AddNewNode<UncheckedNumberOrOddballToFloat64>({node},
                                                         conversion_type);
  } else {
    return AddNewNode<CheckedNumberOrOddballToFloat64>({node}, conversion_type);
  }
}

void MaglevGraphBuilder::BuildCheckSmi(ValueNode* object, bool elidable) {
  if (CheckStaticType(object, NodeType::kSmi)) return;
  if (EnsureType(object, NodeType::kSmi) && elidable) return;
  AddNewNode<CheckSmi>({object});
}

void MaglevGraphBuilder::BuildCheckHeapObject(ValueNode* object) {
  if (EnsureType(object, NodeType::kAnyHeapObject)) return;
  AddNewNode<CheckHeapObject>({object});
}

void MaglevGraphBuilder::BuildCheckString(ValueNode* object) {
  NodeType known_type;
  if (EnsureType(object, NodeType::kString, &known_type)) return;
  AddNewNode<CheckString>({object}, GetCheckType(known_type));
}

void MaglevGraphBuilder::BuildCheckNumber(ValueNode* object) {
  if (EnsureType(object, NodeType::kNumber)) return;
  AddNewNode<CheckNumber>({object}, Object::Conversion::kToNumber);
}

void MaglevGraphBuilder::BuildCheckSymbol(ValueNode* object) {
  NodeType known_type;
  if (EnsureType(object, NodeType::kSymbol, &known_type)) return;
  AddNewNode<CheckSymbol>({object}, GetCheckType(known_type));
}

void MaglevGraphBuilder::BuildCheckJSReceiver(ValueNode* object) {
  NodeType known_type;
  if (EnsureType(object, NodeType::kJSReceiver, &known_type)) return;
  AddNewNode<CheckInstanceType>({object}, GetCheckType(known_type),
                                FIRST_JS_RECEIVER_TYPE, LAST_JS_RECEIVER_TYPE);
}

namespace {

class KnownMapsMerger {
 public:
  explicit KnownMapsMerger(compiler::JSHeapBroker* broker,
                           base::Vector<const compiler::MapRef> requested_maps)
      : broker_(broker), requested_maps_(requested_maps) {}

  void IntersectWithKnownNodeAspects(
      ValueNode* object, const KnownNodeAspects& known_node_aspects) {
    auto node_info_it = known_node_aspects.FindInfo(object);
    bool has_node_info = known_node_aspects.IsValid(node_info_it);
    NodeType type =
        has_node_info ? node_info_it->second.type() : NodeType::kUnknown;
    if (has_node_info && node_info_it->second.possible_maps_are_known()) {
      // TODO(v8:7700): Make intersection non-quadratic.
      for (compiler::MapRef possible_map :
           node_info_it->second.possible_maps()) {
        if (std::find(requested_maps_.begin(), requested_maps_.end(),
                      possible_map) != requested_maps_.end()) {
          // No need to add dependencies, we already have them for all known
          // possible maps.
          // Filter maps which are impossible given this objects type. Since we
          // want to prove that an object with map `map` is not an instance of
          // `type`, we cannot use `StaticTypeForMap`, as it only provides an
          // approximation. This filtering is done to avoid creating
          // non-sensical types later (e.g. if we think only a non-string map
          // is possible, after a string check).
          if (IsInstanceOfNodeType(possible_map, type, broker_)) {
            InsertMap(possible_map);
          }
        } else {
          known_maps_are_subset_of_requested_maps_ = false;
        }
      }
      if (intersect_set_.is_empty()) {
        node_type_ = NodeType::kUnknown;
      }
    } else {
      // A missing entry here means the universal set, i.e., we don't know
      // anything about the possible maps of the object. Intersect with the
      // universal set, which means just insert all requested maps.
      known_maps_are_subset_of_requested_maps_ = false;
      existing_known_maps_found_ = false;
      for (compiler::MapRef map : requested_maps_) {
        InsertMap(map);
      }
    }
  }

  void UpdateKnownNodeAspects(ValueNode* object,
                              KnownNodeAspects& known_node_aspects) {
    // Update known maps.
    auto node_info = known_node_aspects.GetOrCreateInfoFor(object);
    node_info->SetPossibleMaps(intersect_set_, any_map_is_unstable_,
                               node_type_);
    // Make sure known_node_aspects.any_map_for_any_node_is_unstable is updated
    // in case any_map_is_unstable changed to true for this object -- this can
    // happen if this was an intersection with the universal set which added new
    // possible unstable maps.
    if (any_map_is_unstable_) {
      known_node_aspects.any_map_for_any_node_is_unstable = true;
    }
    // At this point, known_node_aspects.any_map_for_any_node_is_unstable may be
    // true despite there no longer being any unstable maps for any nodes (if
    // this was the only node with unstable maps and this intersection removed
    // those). This is ok, because that's at worst just an overestimate -- we
    // could track whether this node's any_map_is_unstable flipped from true to
    // false, but this is likely overkill.
    // Insert stable map dependencies which weren't inserted yet. This is only
    // needed if our set of known maps was empty and we created it anew based on
    // maps we checked.
    if (!existing_known_maps_found_) {
      for (compiler::MapRef map : intersect_set_) {
        if (map.is_stable()) {
          broker_->dependencies()->DependOnStableMap(map);
        }
      }
    } else {
      // TODO(victorgomes): Add a DCHECK_SLOW that checks if the maps already
      // exist in the CompilationDependencySet.
    }
  }

  bool known_maps_are_subset_of_requested_maps() const {
    return known_maps_are_subset_of_requested_maps_;
  }
  bool emit_check_with_migration() const { return emit_check_with_migration_; }

  const compiler::ZoneRefSet<Map>& intersect_set() const {
    return intersect_set_;
  }

  NodeType node_type() const { return node_type_; }

 private:
  compiler::JSHeapBroker* broker_;
  base::Vector<const compiler::MapRef> requested_maps_;
  compiler::ZoneRefSet<Map> intersect_set_;
  bool known_maps_are_subset_of_requested_maps_ = true;
  bool existing_known_maps_found_ = true;
  bool emit_check_with_migration_ = false;
  bool any_map_is_unstable_ = false;
  NodeType node_type_ = static_cast<NodeType>(-1);

  Zone* zone() const { return broker_->zone(); }

  void InsertMap(compiler::MapRef map) {
    if (map.is_migration_target()) {
      emit_check_with_migration_ = true;
    }
    NodeType new_type = StaticTypeForMap(map);
    if (new_type == NodeType::kHeapNumber) {
      new_type = IntersectType(new_type, NodeType::kSmi);
    }
    node_type_ = IntersectType(node_type_, new_type);
    if (!map.is_stable()) {
      any_map_is_unstable_ = true;
    }
    intersect_set_.insert(map, zone());
  }
};

}  // namespace

ReduceResult MaglevGraphBuilder::BuildCheckMaps(
    ValueNode* object, base::Vector<const compiler::MapRef> maps) {
  // TODO(verwaest): Support other objects with possible known stable maps as
  // well.
  if (compiler::OptionalHeapObjectRef constant = TryGetConstant(object)) {
    // For constants with stable maps that match one of the desired maps, we
    // don't need to emit a map check, and can use the dependency -- we
    // can't do this for unstable maps because the constant could migrate
    // during compilation.
    compiler::MapRef constant_map = constant.value().map(broker());
    if (std::find(maps.begin(), maps.end(), constant_map) != maps.end()) {
      if (constant_map.is_stable()) {
        broker()->dependencies()->DependOnStableMap(constant_map);
        return ReduceResult::Done();
      }
      // TODO(verwaest): Reduce maps to the constant map.
    } else {
      // TODO(leszeks): Insert an unconditional deopt if the constant map
      // doesn't match the required map.
    }
  }

  NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(object);
  known_info->CombineType(StaticTypeForNode(broker(), local_isolate(), object));

  // Calculates if known maps are a subset of maps, their map intersection and
  // whether we should emit check with migration.
  KnownMapsMerger merger(broker(), maps);
  merger.IntersectWithKnownNodeAspects(object, known_node_aspects());

  // If the known maps are the subset of the maps to check, we are done.
  if (merger.known_maps_are_subset_of_requested_maps()) {
    // The node type of known_info can get out of sync with the possible maps.
    // For instance after merging with an effectively dead branch (i.e., check
    // contradicting all possible maps).
    // TODO(olivf) Try to combine node_info and possible maps and ensure that
    // narrowing the type also clears impossible possible_maps.
    if (!NodeTypeIs(known_info->type(), merger.node_type())) {
      known_info->IntersectType(merger.node_type());
    }
#ifdef DEBUG
    // Double check that, for every possible map, it's one of the maps we'd
    // want to check.
    for (compiler::MapRef map :
         known_node_aspects().TryGetInfoFor(object)->possible_maps()) {
      DCHECK_NE(std::find(maps.begin(), maps.end(), map), maps.end());
    }
#endif
    return ReduceResult::Done();
  }

  if (merger.intersect_set().is_empty()) {
    return EmitUnconditionalDeopt(DeoptimizeReason::kWrongMap);
  }

  // TODO(v8:7700): Check if the {maps} - {known_maps} size is smaller than
  // {maps} \intersect {known_maps}, we can emit CheckNotMaps instead.

  // Emit checks.
  if (merger.emit_check_with_migration()) {
    AddNewNode<CheckMapsWithMigration>({object}, merger.intersect_set(),
                                       GetCheckType(known_info->type()));
  } else {
    AddNewNode<CheckMaps>({object}, merger.intersect_set(),
                          GetCheckType(known_info->type()));
  }

  merger.UpdateKnownNodeAspects(object, known_node_aspects());
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::BuildTransitionElementsKindOrCheckMap(
    ValueNode* object, base::Vector<const compiler::MapRef> transition_sources,
    compiler::MapRef transition_target) {
  // TODO(marja): Optimizations based on what we know about the intersection of
  // known maps and transition sources or transition target.

  // TransitionElementsKind doesn't happen in cases where we'd need to do
  // CheckMapsWithMigration instead of CheckMaps.
  CHECK(!transition_target.is_migration_target());
  for (const compiler::MapRef transition_source : transition_sources) {
    CHECK(!transition_source.is_migration_target());
  }

  NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(object);
  known_info->CombineType(StaticTypeForNode(broker(), local_isolate(), object));

  AddNewNode<TransitionElementsKindOrCheckMap>(
      {object}, transition_sources, transition_target,
      GetCheckType(known_info->type()));
  // After this operation, object's map is transition_target (or we deopted).
  known_info->SetPossibleMaps(PossibleMaps{transition_target},
                              !transition_target.is_stable(),
                              NodeType::kJSReceiver);
  DCHECK(transition_target.IsJSReceiverMap());
  if (!transition_target.is_stable()) {
    known_node_aspects().any_map_for_any_node_is_unstable = true;
  } else {
    broker()->dependencies()->DependOnStableMap(transition_target);
  }
  return ReduceResult::Done();
}

namespace {
AllocateRaw* GetAllocation(ValueNode* object) {
  if (object->Is<FoldedAllocation>()) {
    object = object->Cast<FoldedAllocation>()->input(0).node();
  }
  if (object->Is<AllocateRaw>()) {
    return object->Cast<AllocateRaw>();
  }
  return nullptr;
}
}  // namespace

bool MaglevGraphBuilder::CanElideWriteBarrier(ValueNode* object,
                                              ValueNode* value) {
  if (value->Is<RootConstant>()) return true;
  if (value->Is<SmiConstant>()) return true;
  if (CheckType(value, NodeType::kSmi)) return true;

  // No need for a write barrier if both object and value are part of the same
  // folded young allocation.
  AllocateRaw* allocation = GetAllocation(object);
  if (allocation != nullptr &&
      allocation->allocation_type() == AllocationType::kYoung &&
      allocation == GetAllocation(value)) {
    return true;
  }

  return false;
}

void MaglevGraphBuilder::BuildStoreTaggedField(ValueNode* object,
                                               ValueNode* value, int offset) {
  if (CanElideWriteBarrier(object, value)) {
    AddNewNode<StoreTaggedFieldNoWriteBarrier>({object, value}, offset);
  } else {
    AddNewNode<StoreTaggedFieldWithWriteBarrier>({object, value}, offset);
  }
}

void MaglevGraphBuilder::BuildStoreTaggedFieldNoWriteBarrier(ValueNode* object,
                                                             ValueNode* value,
                                                             int offset) {
  DCHECK(CanElideWriteBarrier(object, value));
  AddNewNode<StoreTaggedFieldNoWriteBarrier>({object, value}, offset);
}

void MaglevGraphBuilder::BuildStoreFixedArrayElement(ValueNode* elements,
                                                     ValueNode* index,
                                                     ValueNode* value) {
  if (CanElideWriteBarrier(elements, value)) {
    AddNewNode<StoreFixedArrayElementNoWriteBarrier>({elements, index, value});
  } else {
    AddNewNode<StoreFixedArrayElementWithWriteBarrier>(
        {elements, index, value});
  }
}

bool MaglevGraphBuilder::CanTreatHoleAsUndefined(
    base::Vector<const compiler::MapRef> const& receiver_maps) {
  // Check if all {receiver_maps} have one of the initial Array.prototype
  // or Object.prototype objects as their prototype (in any of the current
  // native contexts, as the global Array protector works isolate-wide).
  for (compiler::MapRef receiver_map : receiver_maps) {
    compiler::ObjectRef receiver_prototype = receiver_map.prototype(broker());
    if (!receiver_prototype.IsJSObject() ||
        !broker()->IsArrayOrObjectPrototype(receiver_prototype.AsJSObject())) {
      return false;
    }
  }

  // Check if the array prototype chain is intact.
  return broker()->dependencies()->DependOnNoElementsProtector();
}

compiler::OptionalObjectRef
MaglevGraphBuilder::TryFoldLoadDictPrototypeConstant(
    compiler::PropertyAccessInfo const& access_info) {
  DCHECK(V8_DICT_PROPERTY_CONST_TRACKING_BOOL);
  DCHECK(access_info.IsDictionaryProtoDataConstant());
  DCHECK(access_info.holder().has_value());

  compiler::OptionalObjectRef constant =
      access_info.holder()->GetOwnDictionaryProperty(
          broker(), access_info.dictionary_index(), broker()->dependencies());
  if (!constant.has_value()) return {};

  for (compiler::MapRef map : access_info.lookup_start_object_maps()) {
    Handle<Map> map_handle = map.object();
    // Non-JSReceivers that passed AccessInfoFactory::ComputePropertyAccessInfo
    // must have different lookup start map.
    if (!IsJSReceiverMap(*map_handle)) {
      // Perform the implicit ToObject for primitives here.
      // Implemented according to ES6 section 7.3.2 GetV (V, P).
      JSFunction constructor =
          Map::GetConstructorFunction(
              *map_handle, *broker()->target_native_context().object())
              .value();
      // {constructor.initial_map()} is loaded/stored with acquire-release
      // semantics for constructors.
      map = MakeRefAssumeMemoryFence(broker(), constructor->initial_map());
      DCHECK(IsJSObjectMap(*map.object()));
    }
    broker()->dependencies()->DependOnConstantInDictionaryPrototypeChain(
        map, access_info.name(), constant.value(), PropertyKind::kData);
  }

  return constant;
}

compiler::OptionalObjectRef MaglevGraphBuilder::TryFoldLoadConstantDataField(
    compiler::PropertyAccessInfo const& access_info,
    ValueNode* lookup_start_object) {
  if (!access_info.IsFastDataConstant()) return {};
  compiler::OptionalJSObjectRef source;
  if (access_info.holder().has_value()) {
    source = access_info.holder();
  } else if (compiler::OptionalHeapObjectRef c =
                 TryGetConstant(lookup_start_object)) {
    if (!c.value().IsJSObject()) return {};
    source = c.value().AsJSObject();
  } else {
    return {};
  }
  return source.value().GetOwnFastDataProperty(
      broker(), access_info.field_representation(), access_info.field_index(),
      broker()->dependencies());
}

ReduceResult MaglevGraphBuilder::TryBuildPropertyGetterCall(
    compiler::PropertyAccessInfo const& access_info, ValueNode* receiver,
    ValueNode* lookup_start_object) {
  compiler::ObjectRef constant = access_info.constant().value();

  if (access_info.IsDictionaryProtoAccessorConstant()) {
    // For fast mode holders we recorded dependencies in BuildPropertyLoad.
    for (const compiler::MapRef map : access_info.lookup_start_object_maps()) {
      broker()->dependencies()->DependOnConstantInDictionaryPrototypeChain(
          map, access_info.name(), constant, PropertyKind::kAccessor);
    }
  }

  // Introduce the call to the getter function.
  if (constant.IsJSFunction()) {
    ConvertReceiverMode receiver_mode =
        receiver == lookup_start_object
            ? ConvertReceiverMode::kNotNullOrUndefined
            : ConvertReceiverMode::kAny;
    CallArguments args(receiver_mode, {receiver});
    return ReduceCallForConstant(constant.AsJSFunction(), args);
  } else {
    // Disable optimizations for super ICs using API getters, so that we get
    // the correct receiver checks.
    if (receiver != lookup_start_object) {
      return ReduceResult::Fail();
    }
    compiler::FunctionTemplateInfoRef templ = constant.AsFunctionTemplateInfo();
    CallArguments args(ConvertReceiverMode::kNotNullOrUndefined, {receiver});

    return ReduceCallForApiFunction(templ, {}, access_info.api_holder(), args);
  }
}

ReduceResult MaglevGraphBuilder::TryBuildPropertySetterCall(
    compiler::PropertyAccessInfo const& access_info, ValueNode* receiver,
    ValueNode* value) {
  compiler::ObjectRef constant = access_info.constant().value();
  if (constant.IsJSFunction()) {
    CallArguments args(ConvertReceiverMode::kNotNullOrUndefined,
                       {receiver, value});
    return ReduceCallForConstant(constant.AsJSFunction(), args);
  } else {
    // TODO(victorgomes): API calls.
    return ReduceResult::Fail();
  }
}

ValueNode* MaglevGraphBuilder::BuildLoadField(
    compiler::PropertyAccessInfo const& access_info,
    ValueNode* lookup_start_object) {
  compiler::OptionalObjectRef constant =
      TryFoldLoadConstantDataField(access_info, lookup_start_object);
  if (constant.has_value()) {
    return GetConstant(constant.value());
  }

  // Resolve property holder.
  ValueNode* load_source;
  if (access_info.holder().has_value()) {
    load_source = GetConstant(access_info.holder().value());
  } else {
    load_source = lookup_start_object;
  }

  FieldIndex field_index = access_info.field_index();
  if (!field_index.is_inobject()) {
    // The field is in the property array, first load it from there.
    load_source = AddNewNode<LoadTaggedField>(
        {load_source}, JSReceiver::kPropertiesOrHashOffset);
  }

  // Do the load.
  if (field_index.is_double()) {
    return AddNewNode<LoadDoubleField>({load_source}, field_index.offset());
  }
  ValueNode* value =
      AddNewNode<LoadTaggedField>({load_source}, field_index.offset());
  // Insert stable field information if present.
  if (access_info.field_representation().IsSmi()) {
    NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(value);
    known_info->CombineType(NodeType::kSmi);
  } else if (access_info.field_representation().IsHeapObject()) {
    NodeInfo* known_info = known_node_aspects().GetOrCreateInfoFor(value);
    if (access_info.field_map().has_value() &&
        access_info.field_map().value().is_stable()) {
      DCHECK(access_info.field_map().value().IsJSReceiverMap());
      auto map = access_info.field_map().value();
      known_info->SetPossibleMaps(PossibleMaps{map}, false,
                                  NodeType::kJSReceiver);
      broker()->dependencies()->DependOnStableMap(map);
    } else {
      known_info->CombineType(NodeType::kAnyHeapObject);
    }
  }
  return value;
}

ReduceResult MaglevGraphBuilder::BuildLoadJSArrayLength(ValueNode* js_array) {
  // TODO(leszeks): JSArray.length is known to be non-constant, don't bother
  // searching the constant values.
  RETURN_IF_DONE(
      TryReuseKnownPropertyLoad(js_array, broker()->length_string()));

  ValueNode* length =
      AddNewNode<LoadTaggedField>({js_array}, JSArray::kLengthOffset);
  known_node_aspects().GetOrCreateInfoFor(length)->CombineType(NodeType::kSmi);
  RecordKnownProperty(js_array, broker()->length_string(), length, false,
                      compiler::AccessMode::kLoad);
  return length;
}

void MaglevGraphBuilder::BuildStoreReceiverMap(ValueNode* receiver,
                                               compiler::MapRef map) {
  AddNewNode<StoreMap>({receiver}, map);
  NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(receiver);
  DCHECK(map.IsJSReceiverMap());
  if (map.is_stable()) {
    node_info->SetPossibleMaps(PossibleMaps{map}, false, NodeType::kJSReceiver);
    broker()->dependencies()->DependOnStableMap(map);
  } else {
    node_info->SetPossibleMaps(PossibleMaps{map}, true, NodeType::kJSReceiver);
    known_node_aspects().any_map_for_any_node_is_unstable = true;
  }
}

ReduceResult MaglevGraphBuilder::TryBuildStoreField(
    compiler::PropertyAccessInfo const& access_info, ValueNode* receiver,
    compiler::AccessMode access_mode) {
  FieldIndex field_index = access_info.field_index();
  Representation field_representation = access_info.field_representation();

  if (access_info.HasTransitionMap()) {
    compiler::MapRef transition = access_info.transition_map().value();
    compiler::MapRef original_map = transition.GetBackPointer(broker()).AsMap();
    // TODO(verwaest): Support growing backing stores.
    if (original_map.UnusedPropertyFields() == 0) {
      return ReduceResult::Fail();
    }
  } else if (access_info.IsFastDataConstant() &&
             access_mode == compiler::AccessMode::kStore) {
    return EmitUnconditionalDeopt(DeoptimizeReason::kStoreToConstant);
  }

  ValueNode* store_target;
  if (field_index.is_inobject()) {
    store_target = receiver;
  } else {
    // The field is in the property array, first load it from there.
    store_target = AddNewNode<LoadTaggedField>(
        {receiver}, JSReceiver::kPropertiesOrHashOffset);
  }

  ValueNode* value;
  if (field_representation.IsDouble()) {
    value = GetAccumulatorFloat64();
    if (access_info.HasTransitionMap()) {
      // Allocate the mutable double box owned by the field.
      value = AddNewNode<Float64ToTagged>(
          {value}, Float64ToTagged::ConversionMode::kForceHeapNumber);
    }
  } else {
    if (field_representation.IsSmi()) {
      value = GetAccumulatorSmi();
    } else {
      value = GetAccumulatorTagged();
      if (field_representation.IsHeapObject()) {
        // Emit a map check for the field type, if needed, otherwise just a
        // HeapObject check.
        if (access_info.field_map().has_value()) {
          RETURN_IF_ABORT(BuildCheckMaps(
              value, base::VectorOf({access_info.field_map().value()})));
        } else {
          BuildCheckHeapObject(value);
        }
      }
    }
  }

  if (field_representation.IsSmi()) {
    BuildStoreTaggedFieldNoWriteBarrier(store_target, value,
                                        field_index.offset());
  } else if (value->use_double_register()) {
    DCHECK(field_representation.IsDouble());
    DCHECK(!access_info.HasTransitionMap());
    AddNewNode<StoreDoubleField>({store_target, value}, field_index.offset());
  } else {
    DCHECK(field_representation.IsHeapObject() ||
           field_representation.IsTagged() ||
           (field_representation.IsDouble() && access_info.HasTransitionMap()));
    BuildStoreTaggedField(store_target, value, field_index.offset());
  }

  if (access_info.HasTransitionMap()) {
    BuildStoreReceiverMap(receiver, access_info.transition_map().value());
  }

  return ReduceResult::Done();
}

namespace {
bool AccessInfoGuaranteedConst(
    compiler::PropertyAccessInfo const& access_info) {
  if (!access_info.IsFastDataConstant() && !access_info.IsStringLength()) {
    return false;
  }

  // Even if we have a constant load, if the map is not stable, we cannot
  // guarantee that the load is preserved across side-effecting calls.
  // TODO(v8:7700): It might be possible to track it as const if we know
  // that we're still on the main transition tree; and if we add a
  // dependency on the stable end-maps of the entire tree.
  for (auto& map : access_info.lookup_start_object_maps()) {
    if (!map.is_stable()) {
      return false;
    }
  }
  return true;
}
}  // namespace

ReduceResult MaglevGraphBuilder::TryBuildPropertyLoad(
    ValueNode* receiver, ValueNode* lookup_start_object, compiler::NameRef name,
    compiler::PropertyAccessInfo const& access_info) {
  if (access_info.holder().has_value() && !access_info.HasDictionaryHolder()) {
    broker()->dependencies()->DependOnStablePrototypeChains(
        access_info.lookup_start_object_maps(), kStartAtPrototype,
        access_info.holder().value());
  }

  switch (access_info.kind()) {
    case compiler::PropertyAccessInfo::kInvalid:
      UNREACHABLE();
    case compiler::PropertyAccessInfo::kNotFound:
      return GetRootConstant(RootIndex::kUndefinedValue);
    case compiler::PropertyAccessInfo::kDataField:
    case compiler::PropertyAccessInfo::kFastDataConstant: {
      ValueNode* result = BuildLoadField(access_info, lookup_start_object);
      RecordKnownProperty(lookup_start_object, name, result,
                          AccessInfoGuaranteedConst(access_info),
                          compiler::AccessMode::kLoad);
      return result;
    }
    case compiler::PropertyAccessInfo::kDictionaryProtoDataConstant: {
      compiler::OptionalObjectRef constant =
          TryFoldLoadDictPrototypeConstant(access_info);
      if (!constant.has_value()) return ReduceResult::Fail();
      return GetConstant(constant.value());
    }
    case compiler::PropertyAccessInfo::kFastAccessorConstant:
    case compiler::PropertyAccessInfo::kDictionaryProtoAccessorConstant:
      return TryBuildPropertyGetterCall(access_info, receiver,
                                        lookup_start_object);
    case compiler::PropertyAccessInfo::kModuleExport: {
      ValueNode* cell = GetConstant(access_info.constant().value().AsCell());
      return AddNewNode<LoadTaggedField>({cell}, Cell::kValueOffset);
    }
    case compiler::PropertyAccessInfo::kStringLength: {
      DCHECK_EQ(receiver, lookup_start_object);
      ValueNode* result = AddNewNode<StringLength>({receiver});
      RecordKnownProperty(lookup_start_object, name, result,
                          AccessInfoGuaranteedConst(access_info),
                          compiler::AccessMode::kLoad);
      return result;
    }
  }
}

ReduceResult MaglevGraphBuilder::TryBuildPropertyStore(
    ValueNode* receiver, compiler::NameRef name,
    compiler::PropertyAccessInfo const& access_info,
    compiler::AccessMode access_mode) {
  if (access_info.holder().has_value()) {
    broker()->dependencies()->DependOnStablePrototypeChains(
        access_info.lookup_start_object_maps(), kStartAtPrototype,
        access_info.holder().value());
  }

  if (access_info.IsFastAccessorConstant()) {
    return TryBuildPropertySetterCall(access_info, receiver,
                                      GetAccumulatorTagged());
  } else {
    DCHECK(access_info.IsDataField() || access_info.IsFastDataConstant());
    ReduceResult res = TryBuildStoreField(access_info, receiver, access_mode);
    if (res.IsDone()) {
      RecordKnownProperty(receiver, name,
                          current_interpreter_frame_.accumulator(),
                          AccessInfoGuaranteedConst(access_info), access_mode);
      return res;
    }
    return ReduceResult::Fail();
  }
}

ReduceResult MaglevGraphBuilder::TryBuildPropertyAccess(
    ValueNode* receiver, ValueNode* lookup_start_object, compiler::NameRef name,
    compiler::PropertyAccessInfo const& access_info,
    compiler::AccessMode access_mode) {
  switch (access_mode) {
    case compiler::AccessMode::kLoad:
      return TryBuildPropertyLoad(receiver, lookup_start_object, name,
                                  access_info);
    case compiler::AccessMode::kStore:
    case compiler::AccessMode::kStoreInLiteral:
    case compiler::AccessMode::kDefine:
      DCHECK_EQ(receiver, lookup_start_object);
      return TryBuildPropertyStore(receiver, name, access_info, access_mode);
    case compiler::AccessMode::kHas:
      // TODO(victorgomes): BuildPropertyTest.
      return ReduceResult::Fail();
  }
}

ReduceResult MaglevGraphBuilder::TryBuildNamedAccess(
    ValueNode* receiver, ValueNode* lookup_start_object,
    compiler::NamedAccessFeedback const& feedback,
    compiler::FeedbackSource const& feedback_source,
    compiler::AccessMode access_mode) {
  // Check for the megamorphic case.
  if (feedback.maps().empty()) {
    // We don't have a builtin to fast path megamorphic stores.
    // TODO(leszeks): Maybe we should?
    if (access_mode != compiler::AccessMode::kLoad) return ReduceResult::Fail();
    // We can't do megamorphic loads for lookups where the lookup start isn't
    // the receiver (e.g. load from super).
    if (receiver != lookup_start_object) return ReduceResult::Fail();

    return BuildCallBuiltin<Builtin::kLoadIC_Megamorphic>(
        {receiver, GetConstant(feedback.name())}, feedback_source);
  }

  ZoneVector<compiler::PropertyAccessInfo> access_infos(zone());
  {
    ZoneVector<compiler::PropertyAccessInfo> access_infos_for_feedback(zone());
    compiler::ZoneRefSet<Map> inferred_maps;

    if (compiler::OptionalHeapObjectRef c =
            TryGetConstant(lookup_start_object)) {
      compiler::MapRef constant_map = c.value().map(broker());
      if (c.value().IsJSFunction() &&
          feedback.name().equals(broker()->prototype_string())) {
        compiler::JSFunctionRef function = c.value().AsJSFunction();
        if (!constant_map.has_prototype_slot() ||
            !function.has_instance_prototype(broker()) ||
            function.PrototypeRequiresRuntimeLookup(broker()) ||
            access_mode != compiler::AccessMode::kLoad) {
          return ReduceResult::Fail();
        }
        compiler::HeapObjectRef prototype =
            broker()->dependencies()->DependOnPrototypeProperty(function);
        return GetConstant(prototype);
      }
      inferred_maps = compiler::ZoneRefSet<Map>(constant_map);
    } else {
      // TODO(leszeks): This is doing duplicate work with BuildCheckMaps,
      // consider passing the merger into there.
      KnownMapsMerger merger(broker(), base::VectorOf(feedback.maps()));
      merger.IntersectWithKnownNodeAspects(lookup_start_object,
                                           known_node_aspects());
      inferred_maps = merger.intersect_set();
    }

    if (inferred_maps.is_empty()) {
      return EmitUnconditionalDeopt(DeoptimizeReason::kWrongMap);
    }

    for (compiler::MapRef map : inferred_maps) {
      if (map.is_deprecated()) continue;

      // TODO(v8:12547): Support writing to objects in shared space, which
      // need a write barrier that calls Object::Share to ensure the RHS is
      // shared.
      if (InstanceTypeChecker::IsAlwaysSharedSpaceJSObject(
              map.instance_type()) &&
          access_mode == compiler::AccessMode::kStore) {
        return ReduceResult::Fail();
      }

      compiler::PropertyAccessInfo access_info =
          broker()->GetPropertyAccessInfo(map, feedback.name(), access_mode);
      access_infos_for_feedback.push_back(access_info);
    }

    compiler::AccessInfoFactory access_info_factory(broker(), zone());
    if (!access_info_factory.FinalizePropertyAccessInfos(
            access_infos_for_feedback, access_mode, &access_infos)) {
      return ReduceResult::Fail();
    }
  }

  // Check for monomorphic case.
  if (access_infos.size() == 1) {
    compiler::PropertyAccessInfo access_info = access_infos.front();
    base::Vector<const compiler::MapRef> maps =
        base::VectorOf(access_info.lookup_start_object_maps());
    if (HasOnlyStringMaps(maps)) {
      // Check for string maps before checking if we need to do an access
      // check. Primitive strings always get the prototype from the native
      // context they're operated on, so they don't need the access check.
      BuildCheckString(lookup_start_object);
    } else if (HasOnlyNumberMaps(maps)) {
      BuildCheckNumber(lookup_start_object);
    } else {
      RETURN_IF_ABORT(BuildCheckMaps(lookup_start_object, maps));
    }

    // Generate the actual property
    return TryBuildPropertyAccess(receiver, lookup_start_object,
                                  feedback.name(), access_info, access_mode);
  } else {
    // TODO(victorgomes): Support more generic polymorphic case.

    // Only support polymorphic load at the moment.
    if (access_mode != compiler::AccessMode::kLoad) {
      return ReduceResult::Fail();
    }

    // Check if we support the polymorphic load.
    for (compiler::PropertyAccessInfo const& access_info : access_infos) {
      DCHECK(!access_info.IsInvalid());
      if (access_info.IsDictionaryProtoDataConstant()) {
        compiler::OptionalObjectRef constant =
            access_info.holder()->GetOwnDictionaryProperty(
                broker(), access_info.dictionary_index(),
                broker()->dependencies());
        if (!constant.has_value()) {
          return ReduceResult::Fail();
        }
      } else if (access_info.IsDictionaryProtoAccessorConstant() ||
                 access_info.IsFastAccessorConstant()) {
        return ReduceResult::Fail();
      }

      // TODO(victorgomes): Support map migration.
      for (compiler::MapRef map : access_info.lookup_start_object_maps()) {
        if (map.is_migration_target()) {
          return ReduceResult::Fail();
        }
      }
    }

    // Add compilation dependencies if needed, get constants and fill
    // polymorphic access info.
    Representation field_repr = Representation::Smi();
    ZoneVector<PolymorphicAccessInfo> poly_access_infos(zone());
    poly_access_infos.reserve(access_infos.size());

    for (compiler::PropertyAccessInfo const& access_info : access_infos) {
      if (access_info.holder().has_value() &&
          !access_info.HasDictionaryHolder()) {
        broker()->dependencies()->DependOnStablePrototypeChains(
            access_info.lookup_start_object_maps(), kStartAtPrototype,
            access_info.holder().value());
      }

      const auto& maps = access_info.lookup_start_object_maps();
      switch (access_info.kind()) {
        case compiler::PropertyAccessInfo::kNotFound:
          field_repr = Representation::Tagged();
          poly_access_infos.push_back(PolymorphicAccessInfo::NotFound(maps));
          break;
        case compiler::PropertyAccessInfo::kDataField:
        case compiler::PropertyAccessInfo::kFastDataConstant: {
          field_repr =
              field_repr.generalize(access_info.field_representation());
          compiler::OptionalObjectRef constant =
              TryFoldLoadConstantDataField(access_info, lookup_start_object);
          if (constant.has_value()) {
            poly_access_infos.push_back(
                PolymorphicAccessInfo::Constant(maps, constant.value()));
          } else {
            poly_access_infos.push_back(PolymorphicAccessInfo::DataLoad(
                maps, access_info.field_representation(), access_info.holder(),
                access_info.field_index()));
          }
          break;
        }
        case compiler::PropertyAccessInfo::kDictionaryProtoDataConstant: {
          field_repr =
              field_repr.generalize(access_info.field_representation());
          compiler::OptionalObjectRef constant =
              TryFoldLoadDictPrototypeConstant(access_info);
          DCHECK(constant.has_value());
          poly_access_infos.push_back(
              PolymorphicAccessInfo::Constant(maps, constant.value()));
          break;
        }
        case compiler::PropertyAccessInfo::kModuleExport:
          field_repr = Representation::Tagged();
          break;
        case compiler::PropertyAccessInfo::kStringLength:
          poly_access_infos.push_back(
              PolymorphicAccessInfo::StringLength(maps));
          break;
        default:
          UNREACHABLE();
      }
    }

    if (field_repr.kind() == Representation::kDouble) {
      return AddNewNode<LoadPolymorphicDoubleField>(
          {lookup_start_object}, std::move(poly_access_infos));
    }

    return AddNewNode<LoadPolymorphicTaggedField>(
        {lookup_start_object}, field_repr, std::move(poly_access_infos));
  }
}

ValueNode* MaglevGraphBuilder::GetInt32ElementIndex(ValueNode* object) {
  RecordUseReprHintIfPhi(object, UseRepresentation::kInt32);

  switch (object->properties().value_representation()) {
    case ValueRepresentation::kWord64:
      UNREACHABLE();
    case ValueRepresentation::kTagged:
      NodeType old_type;
      if (SmiConstant* constant = object->TryCast<SmiConstant>()) {
        return GetInt32Constant(constant->value().value());
      } else if (CheckType(object, NodeType::kSmi, &old_type)) {
        auto& alternative =
            known_node_aspects().GetOrCreateInfoFor(object)->alternative();
        return alternative.get_or_set_int32(
            [&]() { return AddNewNode<UnsafeSmiUntag>({object}); });
      } else {
        // TODO(leszeks): Cache this knowledge/converted value somehow on
        // the node info.
        return AddNewNode<CheckedObjectToIndex>({object},
                                                GetCheckType(old_type));
      }
    case ValueRepresentation::kInt32:
      // Already good.
      return object;
    case ValueRepresentation::kUint32:
    case ValueRepresentation::kFloat64:
    case ValueRepresentation::kHoleyFloat64:
      return GetInt32(object);
  }
}

// TODO(victorgomes): Consider caching the values and adding an
// uint32_alternative in node_info.
ValueNode* MaglevGraphBuilder::GetUint32ElementIndex(ValueNode* object) {
  RecordUseReprHintIfPhi(object, UseRepresentation::kUint32);

  switch (object->properties().value_representation()) {
    case ValueRepresentation::kWord64:
      UNREACHABLE();
    case ValueRepresentation::kTagged:
      // TODO(victorgomes): Consider create Uint32Constants and
      // CheckedObjectToUnsignedIndex.
      return AddNewNode<CheckedInt32ToUint32>({GetInt32ElementIndex(object)});
    case ValueRepresentation::kInt32:
      return AddNewNode<CheckedInt32ToUint32>({object});
    case ValueRepresentation::kUint32:
      return object;
    case ValueRepresentation::kFloat64:
      // CheckedTruncateFloat64ToUint32 will gracefully deopt on holes.
    case ValueRepresentation::kHoleyFloat64: {
      return AddNewNode<CheckedTruncateFloat64ToUint32>({object});
    }
  }
}

ReduceResult MaglevGraphBuilder::TryBuildElementAccessOnString(
    ValueNode* object, ValueNode* index_object,
    compiler::KeyedAccessMode const& keyed_mode) {
  // Strings are immutable and `in` cannot be used on strings
  if (keyed_mode.access_mode() != compiler::AccessMode::kLoad) {
    return ReduceResult::Fail();
  }

  // TODO(victorgomes): Deal with LOAD_IGNORE_OUT_OF_BOUNDS.
  if (keyed_mode.load_mode() == LOAD_IGNORE_OUT_OF_BOUNDS) {
    return ReduceResult::Fail();
  }

  DCHECK_EQ(keyed_mode.load_mode(), STANDARD_LOAD);

  // Ensure that {object} is actually a String.
  BuildCheckString(object);

  ValueNode* length = AddNewNode<StringLength>({object});
  ValueNode* index = GetInt32ElementIndex(index_object);
  AddNewNode<CheckInt32Condition>({index, length},
                                  AssertCondition::kUnsignedLessThan,
                                  DeoptimizeReason::kOutOfBounds);

  return AddNewNode<StringAt>({object, index});
}

ValueNode* MaglevGraphBuilder::BuildLoadTypedArrayElement(
    ValueNode* object, ValueNode* index, ElementsKind elements_kind) {
#define BUILD_AND_RETURN_LOAD_TYPED_ARRAY(Type, ...)                       \
  if (broker()->dependencies()->DependOnArrayBufferDetachingProtector()) { \
    return AddNewNode<Load##Type##TypedArrayElementNoDeopt>(__VA_ARGS__);  \
  }                                                                        \
  return AddNewNode<Load##Type##TypedArrayElement>(__VA_ARGS__);
  switch (elements_kind) {
    case INT8_ELEMENTS:
    case INT16_ELEMENTS:
    case INT32_ELEMENTS:
      BUILD_AND_RETURN_LOAD_TYPED_ARRAY(SignedInt, {object, index},
                                        elements_kind);
    case UINT8_CLAMPED_ELEMENTS:
    case UINT8_ELEMENTS:
    case UINT16_ELEMENTS:
    case UINT32_ELEMENTS:
      BUILD_AND_RETURN_LOAD_TYPED_ARRAY(UnsignedInt, {object, index},
                                        elements_kind);
    case FLOAT32_ELEMENTS:
    case FLOAT64_ELEMENTS:
      BUILD_AND_RETURN_LOAD_TYPED_ARRAY(Double, {object, index}, elements_kind);
    default:
      UNREACHABLE();
  }
#undef BUILD_AND_RETURN_LOAD_TYPED_ARRAY
}

void MaglevGraphBuilder::BuildStoreTypedArrayElement(
    ValueNode* object, ValueNode* index, ElementsKind elements_kind) {
#define BUILD_STORE_TYPED_ARRAY(Type, ...)                                 \
  if (broker()->dependencies()->DependOnArrayBufferDetachingProtector()) { \
    AddNewNode<Store##Type##TypedArrayElementNoDeopt>(__VA_ARGS__);        \
  } else {                                                                 \
    AddNewNode<Store##Type##TypedArrayElement>(__VA_ARGS__);               \
  }
  // TODO(leszeks): These operations have a deopt loop when the ToNumber
  // conversion sees a type other than number or oddball. Turbofan has the same
  // deopt loop, but ideally we'd avoid it.
  switch (elements_kind) {
    case UINT8_CLAMPED_ELEMENTS: {
      BUILD_STORE_TYPED_ARRAY(Int,
                              {object, index,
                               GetAccumulatorUint8ClampedForToNumber(
                                   ToNumberHint::kAssumeNumberOrOddball)},
                              elements_kind)
      break;
    }
    case INT8_ELEMENTS:
    case INT16_ELEMENTS:
    case INT32_ELEMENTS:
    case UINT8_ELEMENTS:
    case UINT16_ELEMENTS:
    case UINT32_ELEMENTS:
      BUILD_STORE_TYPED_ARRAY(Int,
                              {object, index,
                               GetAccumulatorTruncatedInt32ForToNumber(
                                   ToNumberHint::kAssumeNumberOrOddball)},
                              elements_kind)
      break;
    case FLOAT32_ELEMENTS:
    case FLOAT64_ELEMENTS:
      BUILD_STORE_TYPED_ARRAY(Double,
                              {object, index,
                               GetAccumulatorHoleyFloat64ForToNumber(
                                   ToNumberHint::kAssumeNumberOrOddball)},
                              elements_kind)
      break;
    default:
      UNREACHABLE();
  }
#undef BUILD_STORE_TYPED_ARRAY
}

ReduceResult MaglevGraphBuilder::TryBuildElementAccessOnTypedArray(
    ValueNode* object, ValueNode* index_object,
    const compiler::ElementAccessInfo& access_info,
    compiler::KeyedAccessMode const& keyed_mode) {
  DCHECK(HasOnlyJSTypedArrayMaps(
      base::VectorOf(access_info.lookup_start_object_maps())));
  ElementsKind elements_kind = access_info.elements_kind();
  if (elements_kind == BIGUINT64_ELEMENTS ||
      elements_kind == BIGINT64_ELEMENTS) {
    return ReduceResult::Fail();
  }
  if (keyed_mode.access_mode() == compiler::AccessMode::kStore &&
      keyed_mode.store_mode() == STORE_IGNORE_OUT_OF_BOUNDS) {
    // TODO(victorgomes): Handle STORE_IGNORE_OUT_OF_BOUNDS mode.
    return ReduceResult::Fail();
  }
  if (keyed_mode.access_mode() == compiler::AccessMode::kStore &&
      elements_kind == UINT8_CLAMPED_ELEMENTS &&
      !IsSupported(CpuOperation::kFloat64Round)) {
    // TODO(victorgomes): Technically we still support if value (in the
    // accumulator) is of type int32. It would be nice to have a roll back
    // mechanism instead, so that we do not need to check this early.
    return ReduceResult::Fail();
  }
  ValueNode* index = GetUint32ElementIndex(index_object);
  AddNewNode<CheckJSTypedArrayBounds>({object, index}, elements_kind);
  switch (keyed_mode.access_mode()) {
    case compiler::AccessMode::kLoad:
      DCHECK_EQ(keyed_mode.load_mode(), STANDARD_LOAD);
      return BuildLoadTypedArrayElement(object, index, elements_kind);
    case compiler::AccessMode::kStore:
      DCHECK_EQ(keyed_mode.store_mode(), STANDARD_STORE);
      BuildStoreTypedArrayElement(object, index, elements_kind);
      return ReduceResult::Done();
    case compiler::AccessMode::kHas:
      // TODO(victorgomes): Implement has element access.
      return ReduceResult::Fail();
    case compiler::AccessMode::kStoreInLiteral:
    case compiler::AccessMode::kDefine:
      UNREACHABLE();
  }
}

ReduceResult MaglevGraphBuilder::TryBuildElementLoadOnJSArrayOrJSObject(
    ValueNode* object, ValueNode* index_object,
    const compiler::ElementAccessInfo& access_info,
    KeyedAccessLoadMode load_mode) {
  ElementsKind elements_kind = access_info.elements_kind();
  DCHECK(IsFastElementsKind(elements_kind));

  base::Vector<const compiler::MapRef> maps =
      base::VectorOf(access_info.lookup_start_object_maps());
  if (IsHoleyElementsKind(elements_kind) && !CanTreatHoleAsUndefined(maps)) {
    return ReduceResult::Fail();
  }

  bool is_jsarray = HasOnlyJSArrayMaps(maps);
  DCHECK(is_jsarray || HasOnlyJSObjectMaps(maps));

  // 1. Get the elements array.
  ValueNode* elements_array =
      AddNewNode<LoadTaggedField>({object}, JSObject::kElementsOffset);

  // 2. Check boundaries,
  ValueNode* index = GetInt32ElementIndex(index_object);
  ValueNode* length_as_smi =
      is_jsarray ? AddNewNode<LoadTaggedField>({object}, JSArray::kLengthOffset)
                 : AddNewNode<LoadTaggedField>({elements_array},
                                               FixedArray::kLengthOffset);
  ValueNode* length = AddNewNode<UnsafeSmiUntag>({length_as_smi});
  AddNewNode<CheckInt32Condition>({index, length},
                                  AssertCondition::kUnsignedLessThan,
                                  DeoptimizeReason::kOutOfBounds);

  // TODO(v8:7700): Add non-deopting bounds check (has to support undefined
  // values).
  DCHECK_EQ(load_mode, STANDARD_LOAD);

  // 3. Do the load.
  ValueNode* result;
  if (elements_kind == HOLEY_DOUBLE_ELEMENTS) {
    result =
        AddNewNode<LoadHoleyFixedDoubleArrayElement>({elements_array, index});
  } else if (elements_kind == PACKED_DOUBLE_ELEMENTS) {
    result = AddNewNode<LoadFixedDoubleArrayElement>({elements_array, index});
  } else {
    DCHECK(!IsDoubleElementsKind(elements_kind));
    result = AddNewNode<LoadFixedArrayElement>({elements_array, index});
    if (IsHoleyElementsKind(elements_kind)) {
      result = AddNewNode<ConvertHoleToUndefined>({result});
    }
  }
  return result;
}

ValueNode* MaglevGraphBuilder::ConvertForStoring(ValueNode* value,
                                                 ElementsKind kind) {
  if (IsDoubleElementsKind(kind)) {
    // Make sure we do not store signalling NaNs into double arrays.
    // TODO(leszeks): Consider making this a bit on StoreFixedDoubleArrayElement
    // rather than a separate node.
    return GetSilencedNaN(GetFloat64(value));
  }
  if (IsSmiElementsKind(kind)) return GetSmiValue(value);
  return GetTaggedValue(value);
}

ReduceResult MaglevGraphBuilder::TryBuildElementStoreOnJSArrayOrJSObject(
    ValueNode* object, ValueNode* index_object, ValueNode* value,
    base::Vector<const compiler::MapRef> maps, ElementsKind elements_kind,
    const compiler::KeyedAccessMode& keyed_mode) {
  DCHECK(IsFastElementsKind(elements_kind));

  const bool is_jsarray = HasOnlyJSArrayMaps(maps);
  DCHECK(is_jsarray || HasOnlyJSObjectMaps(maps));

  // Get the elements array.
  ValueNode* elements_array =
      AddNewNode<LoadTaggedField>({object}, JSObject::kElementsOffset);

  value = ConvertForStoring(value, elements_kind);
  ValueNode* index;

  // TODO(verwaest): Loop peeling will turn the first iteration index of spread
  // literals into smi constants as well, breaking the assumption that we'll
  // have preallocated the space if we see known indices. Turn off this
  // optimization if loop peeling is on.
  if (keyed_mode.access_mode() == compiler::AccessMode::kStoreInLiteral &&
      index_object->Is<SmiConstant>() && is_jsarray &&
      !v8_flags.maglev_loop_peeling) {
    index = GetInt32ElementIndex(index_object);
  } else {
    // Check boundaries.
    ValueNode* elements_array_length = nullptr;
    ValueNode* length;
    if (is_jsarray) {
      length = GetInt32(BuildLoadJSArrayLength(object).value());
    } else {
      length = elements_array_length =
          AddNewNode<UnsafeSmiUntag>({AddNewNode<LoadTaggedField>(
              {elements_array}, FixedArray::kLengthOffset)});
    }
    index = GetInt32ElementIndex(index_object);
    if (keyed_mode.store_mode() == STORE_AND_GROW_HANDLE_COW) {
      if (elements_array_length == nullptr) {
        elements_array_length =
            AddNewNode<UnsafeSmiUntag>({AddNewNode<LoadTaggedField>(
                {elements_array}, FixedArray::kLengthOffset)});
      }

      // Validate the {index} depending on holeyness:
      //
      // For HOLEY_*_ELEMENTS the {index} must not exceed the {elements}
      // backing store capacity plus the maximum allowed gap, as otherwise
      // the (potential) backing store growth would normalize and thus
      // the elements kind of the {receiver} would change to slow mode.
      //
      // For PACKED_*_ELEMENTS the {index} must be within the range
      // [0,length+1[ to be valid. In case {index} equals {length},
      // the {receiver} will be extended, but kept packed.
      ValueNode* limit =
          IsHoleyElementsKind(elements_kind)
              ? AddNewNode<Int32AddWithOverflow>(
                    {elements_array_length,
                     GetInt32Constant(JSObject::kMaxGap)})
              : AddNewNode<Int32AddWithOverflow>({length, GetInt32Constant(1)});
      AddNewNode<CheckInt32Condition>({index, limit},
                                      AssertCondition::kUnsignedLessThan,
                                      DeoptimizeReason::kOutOfBounds);

      // Grow backing store if necessary and handle COW.
      elements_array = AddNewNode<MaybeGrowAndEnsureWritableFastElements>(
          {elements_array, object, index, elements_array_length},
          elements_kind);

      // Update length if necessary.
      if (is_jsarray) {
        AddNewNode<UpdateJSArrayLength>({object, index, length});
        RecordKnownProperty(object, broker()->length_string(), length, false,
                            compiler::AccessMode::kStore);
      }
    } else {
      AddNewNode<CheckInt32Condition>({index, length},
                                      AssertCondition::kUnsignedLessThan,
                                      DeoptimizeReason::kOutOfBounds);

      // Handle COW if needed.
      if (IsSmiOrObjectElementsKind(elements_kind)) {
        if (keyed_mode.store_mode() == STORE_HANDLE_COW) {
          elements_array =
              AddNewNode<EnsureWritableFastElements>({elements_array, object});
        } else {
          // Ensure that this is not a COW FixedArray.
          RETURN_IF_ABORT(BuildCheckMaps(
              elements_array, base::VectorOf({broker()->fixed_array_map()})));
        }
      }
    }
  }

  // Do the store.
  if (IsDoubleElementsKind(elements_kind)) {
    AddNewNode<StoreFixedDoubleArrayElement>({elements_array, index, value});
  } else {
    BuildStoreFixedArrayElement(elements_array, index, value);
  }

  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::TryBuildElementAccessOnJSArrayOrJSObject(
    ValueNode* object, ValueNode* index_object,
    const compiler::ElementAccessInfo& access_info,
    compiler::KeyedAccessMode const& keyed_mode) {
  if (!IsFastElementsKind(access_info.elements_kind())) {
    return ReduceResult::Fail();
  }
  switch (keyed_mode.access_mode()) {
    case compiler::AccessMode::kLoad:
      return TryBuildElementLoadOnJSArrayOrJSObject(
          object, index_object, access_info, keyed_mode.load_mode());
    case compiler::AccessMode::kStoreInLiteral:
    case compiler::AccessMode::kStore: {
      base::Vector<const compiler::MapRef> maps =
          base::VectorOf(access_info.lookup_start_object_maps());
      ElementsKind elements_kind = access_info.elements_kind();
      return TryBuildElementStoreOnJSArrayOrJSObject(object, index_object,
                                                     GetRawAccumulator(), maps,
                                                     elements_kind, keyed_mode);
    }
    default:
      // TODO(victorgomes): Implement more access types.
      return ReduceResult::Fail();
  }
}

ReduceResult MaglevGraphBuilder::TryBuildElementAccess(
    ValueNode* object, ValueNode* index_object,
    compiler::ElementAccessFeedback const& feedback,
    compiler::FeedbackSource const& feedback_source) {
  const compiler::KeyedAccessMode& keyed_mode = feedback.keyed_mode();
  // Check for the megamorphic case.
  if (feedback.transition_groups().empty()) {
    if (keyed_mode.access_mode() != compiler::AccessMode::kLoad) {
      return ReduceResult::Fail();
    }
    return BuildCallBuiltin<Builtin::kKeyedLoadIC_Megamorphic>(
        {object, GetTaggedValue(index_object)}, feedback_source);
  }

  // TODO(leszeks): Add non-deopting bounds check (has to support undefined
  // values).
  if (keyed_mode.access_mode() == compiler::AccessMode::kLoad &&
      keyed_mode.load_mode() != STANDARD_LOAD) {
    return ReduceResult::Fail();
  }

  // TODO(victorgomes): Add fast path for loading from HeapConstant.

  if (feedback.HasOnlyStringMaps(broker())) {
    return TryBuildElementAccessOnString(object, index_object, keyed_mode);
  }

  compiler::AccessInfoFactory access_info_factory(broker(), zone());
  ZoneVector<compiler::ElementAccessInfo> access_infos(zone());
  if (!access_info_factory.ComputeElementAccessInfos(feedback, &access_infos) ||
      access_infos.empty()) {
    return ReduceResult::Fail();
  }

  // TODO(leszeks): This is copied without changes from TurboFan's native
  // context specialization. We should figure out a way to share this code.
  //
  // For holey stores or growing stores, we need to check that the prototype
  // chain contains no setters for elements, and we need to guard those checks
  // via code dependencies on the relevant prototype maps.
  if (keyed_mode.access_mode() == compiler::AccessMode::kStore) {
    // TODO(v8:7700): We could have a fast path here, that checks for the
    // common case of Array or Object prototype only and therefore avoids
    // the zone allocation of this vector.
    ZoneVector<compiler::MapRef> prototype_maps(zone());
    for (compiler::ElementAccessInfo const& access_info : access_infos) {
      for (compiler::MapRef receiver_map :
           access_info.lookup_start_object_maps()) {
        // If the {receiver_map} has a prototype and its elements backing
        // store is either holey, or we have a potentially growing store,
        // then we need to check that all prototypes have stable maps with
        // fast elements (and we need to guard against changes to that below).
        if ((IsHoleyOrDictionaryElementsKind(receiver_map.elements_kind()) ||
             IsGrowStoreMode(feedback.keyed_mode().store_mode())) &&
            !receiver_map.HasOnlyStablePrototypesWithFastElements(
                broker(), &prototype_maps)) {
          return ReduceResult::Fail();
        }

        // TODO(v8:12547): Support writing to objects in shared space, which
        // need a write barrier that calls Object::Share to ensure the RHS is
        // shared.
        if (InstanceTypeChecker::IsAlwaysSharedSpaceJSObject(
                receiver_map.instance_type())) {
          return ReduceResult::Fail();
        }
      }
    }
    for (compiler::MapRef prototype_map : prototype_maps) {
      broker()->dependencies()->DependOnStableMap(prototype_map);
    }
  }

  // Check for monomorphic case.
  if (access_infos.size() == 1) {
    compiler::ElementAccessInfo access_info = access_infos.front();
    // TODO(victorgomes): Support RAB/GSAB backed typed arrays.
    if (IsRabGsabTypedArrayElementsKind(access_info.elements_kind())) {
      return ReduceResult::Fail();
    }

    compiler::MapRef transition_target =
        access_info.lookup_start_object_maps().front();
    if (!access_info.transition_sources().empty()) {
      base::Vector<compiler::MapRef> transition_sources =
          zone()->CloneVector(base::VectorOf(access_info.transition_sources()));
      RETURN_IF_ABORT(BuildTransitionElementsKindOrCheckMap(
          object, transition_sources, transition_target));
    } else {
      RETURN_IF_ABORT(BuildCheckMaps(
          object, base::VectorOf(access_info.lookup_start_object_maps())));
    }
    if (IsTypedArrayElementsKind(access_info.elements_kind())) {
      return TryBuildElementAccessOnTypedArray(object, index_object,
                                               access_info, keyed_mode);
    }
    return TryBuildElementAccessOnJSArrayOrJSObject(object, index_object,
                                                    access_info, keyed_mode);
  } else {
    // TODO(victorgomes): polymorphic case.
    return ReduceResult::Fail();
  }
}

void MaglevGraphBuilder::RecordKnownProperty(ValueNode* lookup_start_object,
                                             compiler::NameRef name,
                                             ValueNode* value, bool is_const,
                                             compiler::AccessMode access_mode) {
  KnownNodeAspects::LoadedPropertyMap& loaded_properties =
      is_const ? known_node_aspects().loaded_constant_properties
               : known_node_aspects().loaded_properties;
  // Try to get loaded_properties[name] if it already exists, otherwise
  // construct loaded_properties[name] = ZoneMap{zone()}.
  auto& props_for_name =
      loaded_properties.try_emplace(name, zone()).first->second;

  if (!is_const && IsAnyStore(access_mode)) {
    // We don't do any aliasing analysis, so stores clobber all other cached
    // loads of a property with that name. We only need to do this for
    // non-constant properties, since constant properties are known not to
    // change and therefore can't be clobbered.
    // TODO(leszeks): Do some light aliasing analysis here, e.g. checking
    // whether there's an intersection of known maps.
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  * Removing all non-constant cached properties with name "
                << *name.object() << std::endl;
    }
    props_for_name.clear();
  }

  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "  * Recording " << (is_const ? "constant" : "non-constant")
              << " known property "
              << PrintNodeLabel(graph_labeller(), lookup_start_object) << ": "
              << PrintNode(graph_labeller(), lookup_start_object) << " ["
              << *name.object()
              << "] = " << PrintNodeLabel(graph_labeller(), value) << ": "
              << PrintNode(graph_labeller(), value) << std::endl;
  }

  props_for_name[lookup_start_object] = value;
}

namespace {
ReduceResult TryFindLoadedProperty(
    const KnownNodeAspects::LoadedPropertyMap& loaded_properties,
    ValueNode* lookup_start_object, compiler::NameRef name) {
  auto props_for_name = loaded_properties.find(name);
  if (props_for_name == loaded_properties.end()) return ReduceResult::Fail();

  auto it = props_for_name->second.find(lookup_start_object);
  if (it == props_for_name->second.end()) return ReduceResult::Fail();

  return it->second;
}
}  // namespace

ReduceResult MaglevGraphBuilder::TryReuseKnownPropertyLoad(
    ValueNode* lookup_start_object, compiler::NameRef name) {
  if (ReduceResult result = TryFindLoadedProperty(
          known_node_aspects().loaded_properties, lookup_start_object, name);
      result.IsDone()) {
    if (v8_flags.trace_maglev_graph_building && result.IsDoneWithValue()) {
      std::cout << "  * Reusing non-constant loaded property "
                << PrintNodeLabel(graph_labeller(), result.value()) << ": "
                << PrintNode(graph_labeller(), result.value()) << std::endl;
    }
    return result;
  }
  if (ReduceResult result =
          TryFindLoadedProperty(known_node_aspects().loaded_constant_properties,
                                lookup_start_object, name);
      result.IsDone()) {
    if (v8_flags.trace_maglev_graph_building && result.IsDoneWithValue()) {
      std::cout << "  * Reusing constant loaded property "
                << PrintNodeLabel(graph_labeller(), result.value()) << ": "
                << PrintNode(graph_labeller(), result.value()) << std::endl;
    }
    return result;
  }
  return ReduceResult::Fail();
}

void MaglevGraphBuilder::VisitGetNamedProperty() {
  // GetNamedProperty <object> <name_index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(feedback_source,
                                             compiler::AccessMode::kLoad, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess));

    case compiler::ProcessedFeedback::kNamedAccess: {
      ReduceResult result = TryReuseKnownPropertyLoad(object, name);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

      result = TryBuildNamedAccess(
          object, object, processed_feedback.AsNamedAccess(), feedback_source,
          compiler::AccessMode::kLoad);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);
      break;
    }
    default:
      break;
  }

  // Create a generic load in the fallthrough.
  ValueNode* context = GetContext();
  SetAccumulator(
      AddNewNode<LoadNamedGeneric>({context, object}, name, feedback_source));
}

ValueNode* MaglevGraphBuilder::GetConstant(compiler::ObjectRef ref) {
  if (ref.IsSmi()) return GetSmiConstant(ref.AsSmi());
  compiler::HeapObjectRef constant = ref.AsHeapObject();

  if (IsThinString(*constant.object())) {
    constant = MakeRefAssumeMemoryFence(
        broker(), ThinString::cast(*constant.object())->actual());
  }

  auto root_index = broker()->FindRootIndex(constant);
  if (root_index.has_value()) {
    return GetRootConstant(*root_index);
  }

  auto it = graph_->constants().find(constant);
  if (it == graph_->constants().end()) {
    Constant* node = CreateNewConstantNode<Constant>(0, constant);
    if (has_graph_labeller()) graph_labeller()->RegisterNode(node);
    graph_->constants().emplace(constant, node);
    return node;
  }
  return it->second;
}

void MaglevGraphBuilder::VisitGetNamedPropertyFromSuper() {
  // GetNamedPropertyFromSuper <receiver> <name_index> <slot>
  ValueNode* receiver = LoadRegisterTagged(0);
  ValueNode* home_object = GetAccumulatorTagged();
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  // {home_object} is guaranteed to be a HeapObject.
  ValueNode* home_object_map =
      AddNewNode<LoadTaggedField>({home_object}, HeapObject::kMapOffset);
  ValueNode* lookup_start_object =
      AddNewNode<LoadTaggedField>({home_object_map}, Map::kPrototypeOffset);

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(feedback_source,
                                             compiler::AccessMode::kLoad, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess));

    case compiler::ProcessedFeedback::kNamedAccess: {
      ReduceResult result =
          TryReuseKnownPropertyLoad(lookup_start_object, name);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

      result = TryBuildNamedAccess(
          receiver, lookup_start_object, processed_feedback.AsNamedAccess(),
          feedback_source, compiler::AccessMode::kLoad);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);
      break;
    }
    default:
      break;
  }

  // Create a generic load.
  ValueNode* context = GetContext();
  SetAccumulator(AddNewNode<LoadNamedFromSuperGeneric>(
      {context, receiver, lookup_start_object}, name, feedback_source));
}

void MaglevGraphBuilder::VisitGetKeyedProperty() {
  // GetKeyedProperty <object> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  // TODO(leszeks): We don't need to tag the key if it's an Int32 and a simple
  // monomorphic element load.
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kLoad, base::nullopt);

  if (current_for_in_state.index != nullptr &&
      current_for_in_state.receiver == object &&
      current_for_in_state.key == current_interpreter_frame_.accumulator()) {
    if (current_for_in_state.receiver_needs_map_check) {
      auto* receiver_map =
          AddNewNode<LoadTaggedField>({object}, HeapObject::kMapOffset);
      AddNewNode<CheckDynamicValue>(
          {receiver_map, current_for_in_state.cache_type});
      current_for_in_state.receiver_needs_map_check = false;
    }
    // TODO(leszeks): Cache the indices across the loop.
    auto* cache_array = AddNewNode<LoadTaggedField>(
        {current_for_in_state.enum_cache}, EnumCache::kIndicesOffset);
    AddNewNode<CheckFixedArrayNonEmpty>({cache_array});
    // TODO(leszeks): Cache the field index per iteration.
    auto* field_index = AddNewNode<LoadFixedArrayElement>(
        {cache_array, current_for_in_state.index});
    SetAccumulator(
        AddNewNode<LoadTaggedFieldByFieldIndex>({object, field_index}));
    return;
  }

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericKeyedAccess));

    case compiler::ProcessedFeedback::kElementAccess: {
      // Get the accumulator without conversion. TryBuildElementAccess
      // will try to pick the best representation.
      ValueNode* index = current_interpreter_frame_.accumulator();
      ReduceResult result = TryBuildElementAccess(
          object, index, processed_feedback.AsElementAccess(), feedback_source);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);
      break;
    }

    case compiler::ProcessedFeedback::kNamedAccess: {
      ValueNode* key = GetAccumulatorTagged();
      compiler::NameRef name = processed_feedback.AsNamedAccess().name();
      RETURN_VOID_IF_ABORT(BuildCheckValue(key, name));

      ReduceResult result = TryReuseKnownPropertyLoad(object, name);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

      result = TryBuildNamedAccess(
          object, object, processed_feedback.AsNamedAccess(), feedback_source,
          compiler::AccessMode::kLoad);
      PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);
      break;
    }

    default:
      break;
  }

  // Create a generic store in the fallthrough.
  ValueNode* context = GetContext();
  ValueNode* key = GetAccumulatorTagged();
  SetAccumulator(
      AddNewNode<GetKeyedGeneric>({context, object, key}, feedback_source));
}

void MaglevGraphBuilder::VisitLdaModuleVariable() {
  // LdaModuleVariable <cell_index> <depth>
  int cell_index = iterator_.GetImmediateOperand(0);
  size_t depth = iterator_.GetUnsignedImmediateOperand(1);

  ValueNode* context = GetContext();
  MinimizeContextChainDepth(&context, &depth);

  if (compilation_unit_->info()->specialize_to_function_context()) {
    compiler::OptionalContextRef maybe_ref =
        FunctionContextSpecialization::TryToRef(compilation_unit_, context,
                                                &depth);
    if (maybe_ref.has_value()) {
      context = GetConstant(maybe_ref.value());
    }
  }

  for (size_t i = 0; i < depth; i++) {
    context = LoadAndCacheContextSlot(
        context, Context::OffsetOfElementAt(Context::PREVIOUS_INDEX),
        kImmutable);
  }
  ValueNode* module = LoadAndCacheContextSlot(
      context, Context::OffsetOfElementAt(Context::EXTENSION_INDEX),
      kImmutable);
  ValueNode* exports_or_imports;
  if (cell_index > 0) {
    exports_or_imports = AddNewNode<LoadTaggedField>(
        {module}, SourceTextModule::kRegularExportsOffset);
    // The actual array index is (cell_index - 1).
    cell_index -= 1;
  } else {
    exports_or_imports = AddNewNode<LoadTaggedField>(
        {module}, SourceTextModule::kRegularImportsOffset);
    // The actual array index is (-cell_index - 1).
    cell_index = -cell_index - 1;
  }
  ValueNode* cell = BuildLoadFixedArrayElement(exports_or_imports, cell_index);
  SetAccumulator(AddNewNode<LoadTaggedField>({cell}, Cell::kValueOffset));
}

void MaglevGraphBuilder::VisitStaModuleVariable() {
  // StaModuleVariable <cell_index> <depth>
  int cell_index = iterator_.GetImmediateOperand(0);
  if (V8_UNLIKELY(cell_index < 0)) {
    BuildCallRuntime(Runtime::kAbort,
                     {GetSmiConstant(static_cast<int>(
                         AbortReason::kUnsupportedModuleOperation))});
    return;
  }

  ValueNode* context = GetContext();
  size_t depth = iterator_.GetUnsignedImmediateOperand(1);
  MinimizeContextChainDepth(&context, &depth);

  if (compilation_unit_->info()->specialize_to_function_context()) {
    compiler::OptionalContextRef maybe_ref =
        FunctionContextSpecialization::TryToRef(compilation_unit_, context,
                                                &depth);
    if (maybe_ref.has_value()) {
      context = GetConstant(maybe_ref.value());
    }
  }

  for (size_t i = 0; i < depth; i++) {
    context = LoadAndCacheContextSlot(
        context, Context::OffsetOfElementAt(Context::PREVIOUS_INDEX),
        kImmutable);
  }
  ValueNode* module = LoadAndCacheContextSlot(
      context, Context::OffsetOfElementAt(Context::EXTENSION_INDEX),
      kImmutable);
  ValueNode* exports = AddNewNode<LoadTaggedField>(
      {module}, SourceTextModule::kRegularExportsOffset);
  // The actual array index is (cell_index - 1).
  cell_index -= 1;
  ValueNode* cell = BuildLoadFixedArrayElement(exports, cell_index);
  BuildStoreTaggedField(cell, GetAccumulatorTagged(), Cell::kValueOffset);
}

void MaglevGraphBuilder::BuildLoadGlobal(
    compiler::NameRef name, compiler::FeedbackSource& feedback_source,
    TypeofMode typeof_mode) {
  const compiler::ProcessedFeedback& access_feedback =
      broker()->GetFeedbackForGlobalAccess(feedback_source);

  if (access_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForGenericGlobalAccess));
  }

  const compiler::GlobalAccessFeedback& global_access_feedback =
      access_feedback.AsGlobalAccess();
  PROCESS_AND_RETURN_IF_DONE(TryBuildGlobalLoad(global_access_feedback),
                             SetAccumulator);

  ValueNode* context = GetContext();
  SetAccumulator(
      AddNewNode<LoadGlobal>({context}, name, feedback_source, typeof_mode));
}

void MaglevGraphBuilder::VisitSetNamedProperty() {
  // SetNamedProperty <object> <name_index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kStore, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess));

    case compiler::ProcessedFeedback::kNamedAccess:
      RETURN_VOID_IF_DONE(TryBuildNamedAccess(
          object, object, processed_feedback.AsNamedAccess(), feedback_source,
          compiler::AccessMode::kStore));
      break;
    default:
      break;
  }

  // Create a generic store in the fallthrough.
  ValueNode* context = GetContext();
  ValueNode* value = GetAccumulatorTagged();
  AddNewNode<SetNamedGeneric>({context, object, value}, name, feedback_source);
}

void MaglevGraphBuilder::VisitDefineNamedOwnProperty() {
  // DefineNamedOwnProperty <object> <name_index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  compiler::NameRef name = GetRefOperand<Name>(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kStore, name);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericNamedAccess));

    case compiler::ProcessedFeedback::kNamedAccess:
      RETURN_VOID_IF_DONE(TryBuildNamedAccess(
          object, object, processed_feedback.AsNamedAccess(), feedback_source,
          compiler::AccessMode::kDefine));
      break;

    default:
      break;
  }

  // Create a generic store in the fallthrough.
  ValueNode* context = GetContext();
  ValueNode* value = GetAccumulatorTagged();
  AddNewNode<DefineNamedOwnGeneric>({context, object, value}, name,
                                    feedback_source);
}

void MaglevGraphBuilder::VisitSetKeyedProperty() {
  // SetKeyedProperty <object> <key> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kLoad, base::nullopt);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericKeyedAccess));

    case compiler::ProcessedFeedback::kElementAccess: {
      // Get the key without conversion. TryBuildElementAccess will try to pick
      // the best representation.
      ValueNode* index =
          current_interpreter_frame_.get(iterator_.GetRegisterOperand(1));
      RETURN_VOID_IF_DONE(TryBuildElementAccess(
          object, index, processed_feedback.AsElementAccess(),
          feedback_source));
    } break;

    default:
      break;
  }

  // Create a generic store in the fallthrough.
  ValueNode* key = LoadRegisterTagged(1);
  ValueNode* context = GetContext();
  ValueNode* value = GetAccumulatorTagged();
  AddNewNode<SetKeyedGeneric>({context, object, key, value}, feedback_source);
}

void MaglevGraphBuilder::VisitDefineKeyedOwnProperty() {
  // DefineKeyedOwnProperty <object> <key> <flags> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* key = LoadRegisterTagged(1);
  ValueNode* flags = GetSmiConstant(GetFlag8Operand(2));
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  // TODO(victorgomes): Add monomorphic fast path.

  // Create a generic store in the fallthrough.
  ValueNode* context = GetContext();
  ValueNode* value = GetAccumulatorTagged();
  AddNewNode<DefineKeyedOwnGeneric>({context, object, key, value, flags},
                                    feedback_source);
}

void MaglevGraphBuilder::VisitStaInArrayLiteral() {
  // StaInArrayLiteral <object> <index> <slot>
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* index = LoadRegisterRaw(1);
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForPropertyAccess(
          feedback_source, compiler::AccessMode::kStoreInLiteral,
          base::nullopt);

  switch (processed_feedback.kind()) {
    case compiler::ProcessedFeedback::kInsufficient:
      RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
          DeoptimizeReason::kInsufficientTypeFeedbackForGenericKeyedAccess));

    case compiler::ProcessedFeedback::kElementAccess: {
      RETURN_VOID_IF_DONE(TryBuildElementAccess(
          object, index, processed_feedback.AsElementAccess(),
          feedback_source));
      break;
    }

    default:
      break;
  }

  // Create a generic store in the fallthrough.
  ValueNode* context = GetContext();
  ValueNode* value = GetAccumulatorTagged();
  AddNewNode<StoreInArrayLiteralGeneric>(
      {context, object, GetTaggedValue(index), value}, feedback_source);
}

void MaglevGraphBuilder::VisitDefineKeyedOwnPropertyInLiteral() {
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* name = LoadRegisterTagged(1);
  ValueNode* value = GetAccumulatorTagged();
  ValueNode* flags = GetSmiConstant(GetFlag8Operand(2));
  ValueNode* slot = GetSmiConstant(GetSlotOperand(3).ToInt());
  ValueNode* feedback_vector = GetConstant(feedback());
  BuildCallRuntime(Runtime::kDefineKeyedOwnPropertyInLiteral,
                   {object, name, value, flags, feedback_vector, slot});
}

void MaglevGraphBuilder::VisitAdd() { VisitBinaryOperation<Operation::kAdd>(); }
void MaglevGraphBuilder::VisitSub() {
  VisitBinaryOperation<Operation::kSubtract>();
}
void MaglevGraphBuilder::VisitMul() {
  VisitBinaryOperation<Operation::kMultiply>();
}
void MaglevGraphBuilder::VisitDiv() {
  VisitBinaryOperation<Operation::kDivide>();
}
void MaglevGraphBuilder::VisitMod() {
  VisitBinaryOperation<Operation::kModulus>();
}
void MaglevGraphBuilder::VisitExp() {
  VisitBinaryOperation<Operation::kExponentiate>();
}
void MaglevGraphBuilder::VisitBitwiseOr() {
  VisitBinaryOperation<Operation::kBitwiseOr>();
}
void MaglevGraphBuilder::VisitBitwiseXor() {
  VisitBinaryOperation<Operation::kBitwiseXor>();
}
void MaglevGraphBuilder::VisitBitwiseAnd() {
  VisitBinaryOperation<Operation::kBitwiseAnd>();
}
void MaglevGraphBuilder::VisitShiftLeft() {
  VisitBinaryOperation<Operation::kShiftLeft>();
}
void MaglevGraphBuilder::VisitShiftRight() {
  VisitBinaryOperation<Operation::kShiftRight>();
}
void MaglevGraphBuilder::VisitShiftRightLogical() {
  VisitBinaryOperation<Operation::kShiftRightLogical>();
}

void MaglevGraphBuilder::VisitAddSmi() {
  VisitBinarySmiOperation<Operation::kAdd>();
}
void MaglevGraphBuilder::VisitSubSmi() {
  VisitBinarySmiOperation<Operation::kSubtract>();
}
void MaglevGraphBuilder::VisitMulSmi() {
  VisitBinarySmiOperation<Operation::kMultiply>();
}
void MaglevGraphBuilder::VisitDivSmi() {
  VisitBinarySmiOperation<Operation::kDivide>();
}
void MaglevGraphBuilder::VisitModSmi() {
  VisitBinarySmiOperation<Operation::kModulus>();
}
void MaglevGraphBuilder::VisitExpSmi() {
  VisitBinarySmiOperation<Operation::kExponentiate>();
}
void MaglevGraphBuilder::VisitBitwiseOrSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseOr>();
}
void MaglevGraphBuilder::VisitBitwiseXorSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseXor>();
}
void MaglevGraphBuilder::VisitBitwiseAndSmi() {
  VisitBinarySmiOperation<Operation::kBitwiseAnd>();
}
void MaglevGraphBuilder::VisitShiftLeftSmi() {
  VisitBinarySmiOperation<Operation::kShiftLeft>();
}
void MaglevGraphBuilder::VisitShiftRightSmi() {
  VisitBinarySmiOperation<Operation::kShiftRight>();
}
void MaglevGraphBuilder::VisitShiftRightLogicalSmi() {
  VisitBinarySmiOperation<Operation::kShiftRightLogical>();
}

void MaglevGraphBuilder::VisitInc() {
  VisitUnaryOperation<Operation::kIncrement>();
}
void MaglevGraphBuilder::VisitDec() {
  VisitUnaryOperation<Operation::kDecrement>();
}
void MaglevGraphBuilder::VisitNegate() {
  VisitUnaryOperation<Operation::kNegate>();
}
void MaglevGraphBuilder::VisitBitwiseNot() {
  VisitUnaryOperation<Operation::kBitwiseNot>();
}

void MaglevGraphBuilder::VisitToBooleanLogicalNot() {
  BuildToBoolean</* flip */ true>(GetAccumulatorTagged());
}

void MaglevGraphBuilder::VisitLogicalNot() {
  // Invariant: accumulator must already be a boolean value.
  ValueNode* value = GetAccumulatorTagged();
  switch (value->opcode()) {
#define CASE(Name)                                                             \
  case Opcode::k##Name: {                                                      \
    SetAccumulator(                                                            \
        GetBooleanConstant(!value->Cast<Name>()->ToBoolean(local_isolate()))); \
    break;                                                                     \
  }
    CONSTANT_VALUE_NODE_LIST(CASE)
#undef CASE
    default:
      SetAccumulator(AddNewNode<LogicalNot>({value}));
      break;
  }
}

void MaglevGraphBuilder::VisitTypeOf() {
  ValueNode* value = GetAccumulatorTagged();
  SetAccumulator(BuildCallBuiltin<Builtin::kTypeof>({value}));
}

void MaglevGraphBuilder::VisitDeletePropertyStrict() {
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* key = GetAccumulatorTagged();
  ValueNode* context = GetContext();
  SetAccumulator(AddNewNode<DeleteProperty>({context, object, key},
                                            LanguageMode::kStrict));
}

void MaglevGraphBuilder::VisitDeletePropertySloppy() {
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* key = GetAccumulatorTagged();
  ValueNode* context = GetContext();
  SetAccumulator(AddNewNode<DeleteProperty>({context, object, key},
                                            LanguageMode::kSloppy));
}

void MaglevGraphBuilder::VisitGetSuperConstructor() {
  ValueNode* active_function = GetAccumulatorTagged();
  ValueNode* map_proto;
  if (compiler::OptionalHeapObjectRef constant =
          TryGetConstant(active_function)) {
    map_proto = GetConstant(constant->map(broker()).prototype(broker()));
  } else {
    ValueNode* map =
        AddNewNode<LoadTaggedField>({active_function}, HeapObject::kMapOffset);
    map_proto = AddNewNode<LoadTaggedField>({map}, Map::kPrototypeOffset);
  }
  StoreRegister(iterator_.GetRegisterOperand(0), map_proto);
}

bool MaglevGraphBuilder::HasValidInitialMap(
    compiler::JSFunctionRef new_target, compiler::JSFunctionRef constructor) {
  if (!new_target.map(broker()).has_prototype_slot()) return false;
  if (!new_target.has_initial_map(broker())) return false;
  compiler::MapRef initial_map = new_target.initial_map(broker());
  return initial_map.GetConstructor(broker()).equals(constructor);
}

void MaglevGraphBuilder::VisitFindNonDefaultConstructorOrConstruct() {
  ValueNode* this_function = LoadRegisterTagged(0);
  ValueNode* new_target = LoadRegisterTagged(1);

  auto register_pair = iterator_.GetRegisterPairOperand(2);

  if (compiler::OptionalHeapObjectRef constant =
          TryGetConstant(this_function)) {
    compiler::MapRef function_map = constant->map(broker());
    compiler::HeapObjectRef current = function_map.prototype(broker());

    if (broker()->dependencies()->DependOnArrayIteratorProtector()) {
      while (true) {
        if (!current.IsJSFunction()) break;
        compiler::JSFunctionRef current_function = current.AsJSFunction();
        if (current_function.shared(broker())
                .requires_instance_members_initializer()) {
          break;
        }
        if (current_function.context(broker())
                .scope_info(broker())
                .ClassScopeHasPrivateBrand()) {
          break;
        }
        FunctionKind kind = current_function.shared(broker()).kind();
        if (kind != FunctionKind::kDefaultDerivedConstructor) {
          broker()->dependencies()->DependOnStablePrototypeChain(
              function_map, WhereToStart::kStartAtReceiver, current_function);

          compiler::OptionalHeapObjectRef new_target_function =
              TryGetConstant(new_target);
          if (kind == FunctionKind::kDefaultBaseConstructor) {
            ValueNode* object;
            if (new_target_function && new_target_function->IsJSFunction() &&
                HasValidInitialMap(new_target_function->AsJSFunction(),
                                   current_function)) {
              object = BuildAllocateFastObject(
                  FastObject(new_target_function->AsJSFunction(), zone(),
                             broker()),
                  AllocationType::kYoung);
            } else {
              object = BuildCallBuiltin<Builtin::kFastNewObject>(
                  {GetConstant(current_function), new_target});
            }
            StoreRegister(register_pair.first, GetBooleanConstant(true));
            StoreRegister(register_pair.second, object);
            return;
          }
          break;
        }

        // Keep walking up the class tree.
        current = current_function.map(broker()).prototype(broker());
      }
    }
    StoreRegister(register_pair.first, GetBooleanConstant(false));
    StoreRegister(register_pair.second, GetConstant(current));
    return;
  }

  CallBuiltin* result =
      BuildCallBuiltin<Builtin::kFindNonDefaultConstructorOrConstruct>(
          {this_function, new_target});
  StoreRegisterPair(register_pair, result);
}

ReduceResult MaglevGraphBuilder::BuildInlined(ValueNode* context,
                                              ValueNode* function,
                                              ValueNode* new_target,
                                              const CallArguments& args) {
  DCHECK(is_inline());

  // Manually create the prologue of the inner function graph, so that we
  // can manually set up the arguments.
  DCHECK_NOT_NULL(current_block_);

  // Set receiver.
  ValueNode* receiver =
      GetRawConvertReceiver(compilation_unit_->shared_function_info(), args);
  SetArgument(0, receiver);
  // Set remaining arguments.
  RootConstant* undefined_constant =
      GetRootConstant(RootIndex::kUndefinedValue);
  for (int i = 1; i < parameter_count(); i++) {
    ValueNode* arg_value = args[i - 1];
    if (arg_value == nullptr) arg_value = undefined_constant;
    SetArgument(i, arg_value);
  }

  int arg_count = static_cast<int>(args.count());
  int formal_parameter_count =
      compilation_unit_->shared_function_info()
          .internal_formal_parameter_count_without_receiver();
  if (arg_count != formal_parameter_count) {
    inlined_arguments_.emplace(
        zone()->AllocateVector<ValueNode*>(arg_count + 1));
    (*inlined_arguments_)[0] = receiver;
    for (int i = 0; i < arg_count; i++) {
      (*inlined_arguments_)[i + 1] = args[i];
    }
  }
  inlined_new_target_ = new_target;

  BuildRegisterFrameInitialization(context, function, new_target);
  BuildMergeStates();
  EndPrologue();

  // Build the inlined function body.
  BuildBody();

  if (terminated_with_fused_branch_) {
    // If we fused a test in the inlined function with a branch in the caller,
    // the basic blocks are already wired correctly and there is no merge point
    // to handle.
    DCHECK_NULL(current_block_);
    DCHECK_NULL(merge_states_[inline_exit_offset()]);
    return ReduceResult::Done();
  }

  // All returns in the inlined body jump to a merge point one past the bytecode
  // length (i.e. at offset bytecode.length()). If there isn't one already,
  // create a block at this fake offset and have it jump out of the inlined
  // function, into a new block that we create which resumes execution of the
  // outer function.
  if (!current_block_) {
    // If we don't have a merge state at the inline_exit_offset, then there is
    // no control flow that reaches the end of the inlined function, either
    // because of infinite loops or deopts
    if (merge_states_[inline_exit_offset()] == nullptr) {
      return ReduceResult::DoneWithAbort();
    }

    ProcessMergePoint(inline_exit_offset());
    StartNewBlock(inline_exit_offset(), /*predecessor*/ nullptr);
  }

  // Pull the returned accumulator value out of the inlined function's final
  // merged return state.
  return current_interpreter_frame_.accumulator();
}

#define TRACE_INLINING(...)                       \
  do {                                            \
    if (v8_flags.trace_maglev_inlining)           \
      StdoutStream{} << __VA_ARGS__ << std::endl; \
  } while (false)

#define TRACE_CANNOT_INLINE(...) \
  TRACE_INLINING("  cannot inline " << shared << ": " << __VA_ARGS__)

bool MaglevGraphBuilder::ShouldInlineCall(
    compiler::SharedFunctionInfoRef shared,
    compiler::OptionalFeedbackVectorRef feedback_vector, float call_frequency) {
  if (graph()->total_inlined_bytecode_size() >
      v8_flags.max_maglev_inlined_bytecode_size_cumulative) {
    TRACE_CANNOT_INLINE("maximum inlined bytecode size");
    return false;
  }
  if (!feedback_vector) {
    // TODO(verwaest): Soft deopt instead?
    TRACE_CANNOT_INLINE("it has not been compiled/run with feedback yet");
    return false;
  }
  if (compilation_unit_->shared_function_info().equals(shared)) {
    TRACE_CANNOT_INLINE("direct recursion");
    return false;
  }
  SharedFunctionInfo::Inlineability inlineability =
      shared.GetInlineability(broker());
  if (inlineability != SharedFunctionInfo::Inlineability::kIsInlineable) {
    TRACE_CANNOT_INLINE(inlineability);
    return false;
  }
  // TODO(victorgomes): Support NewTarget/RegisterInput in inlined functions.
  compiler::BytecodeArrayRef bytecode = shared.GetBytecodeArray(broker());
  if (bytecode.incoming_new_target_or_generator_register().is_valid()) {
    TRACE_CANNOT_INLINE("use unsupported NewTargetOrGenerator register");
    return false;
  }
  // TODO(victorgomes): Support exception handler inside inlined functions.
  if (bytecode.handler_table_size() > 0) {
    TRACE_CANNOT_INLINE("use unsupported expection handlers");
    return false;
  }
  // TODO(victorgomes): Support inlined allocation of the arguments object.
  interpreter::BytecodeArrayIterator iterator(bytecode.object());
  for (; !iterator.done(); iterator.Advance()) {
    switch (iterator.current_bytecode()) {
      case interpreter::Bytecode::kCreateMappedArguments:
      case interpreter::Bytecode::kCreateUnmappedArguments:
      case interpreter::Bytecode::kCreateRestParameter:
        TRACE_CANNOT_INLINE("not supported inlined arguments object");
        return false;
      default:
        break;
    }
  }
  if (call_frequency < v8_flags.min_maglev_inlining_frequency) {
    TRACE_CANNOT_INLINE("call frequency ("
                        << call_frequency << ") < minimum threshold ("
                        << v8_flags.min_maglev_inlining_frequency << ")");
    return false;
  }
  if (bytecode.length() < v8_flags.max_maglev_inlined_bytecode_size_small) {
    TRACE_INLINING("  inlining " << shared << ": small function");
    return true;
  }
  if (bytecode.length() > v8_flags.max_maglev_inlined_bytecode_size) {
    TRACE_CANNOT_INLINE("big function, size ("
                        << bytecode.length() << ") >= max-size ("
                        << v8_flags.max_maglev_inlined_bytecode_size << ")");
    return false;
  }
  if (inlining_depth() > v8_flags.max_maglev_inline_depth) {
    TRACE_CANNOT_INLINE("inlining depth ("
                        << inlining_depth() << ") >= max-depth ("
                        << v8_flags.max_maglev_inline_depth << ")");
    return false;
  }
  TRACE_INLINING("  inlining " << shared);
  if (v8_flags.trace_maglev_inlining_verbose) {
    BytecodeArray::Disassemble(bytecode.object(), std::cout);
    i::Print(*feedback_vector->object(), std::cout);
  }
  graph()->add_inlined_bytecode_size(bytecode.length());
  return true;
}

ReduceResult MaglevGraphBuilder::TryBuildInlinedCall(
    ValueNode* context, ValueNode* function, ValueNode* new_target,
    compiler::SharedFunctionInfoRef shared,
    compiler::OptionalFeedbackVectorRef feedback_vector, CallArguments& args,
    const compiler::FeedbackSource& feedback_source) {
  DCHECK_EQ(args.mode(), CallArguments::kDefault);
  float feedback_frequency = 0.0f;
  if (feedback_source.IsValid()) {
    compiler::ProcessedFeedback const& feedback =
        broker()->GetFeedbackForCall(feedback_source);
    feedback_frequency =
        feedback.IsInsufficient() ? 0.0f : feedback.AsCall().frequency();
  }
  float call_frequency = feedback_frequency * call_frequency_;
  if (!ShouldInlineCall(shared, feedback_vector, call_frequency)) {
    return ReduceResult::Fail();
  }

  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "== Inlining " << shared.object() << std::endl;
  }

  compiler::BytecodeArrayRef bytecode = shared.GetBytecodeArray(broker());
  graph()->inlined_functions().push_back(
      OptimizedCompilationInfo::InlinedFunctionHolder(
          shared.object(), bytecode.object(), current_source_position_));
  int inlining_id = static_cast<int>(graph()->inlined_functions().size() - 1);

  // Create a new compilation unit and graph builder for the inlined
  // function.
  MaglevCompilationUnit* inner_unit = MaglevCompilationUnit::NewInner(
      zone(), compilation_unit_, shared, feedback_vector.value());
  MaglevGraphBuilder inner_graph_builder(
      local_isolate_, inner_unit, graph_, call_frequency,
      BytecodeOffset(iterator_.current_offset()), inlining_id, this);

  // Propagate catch block.
  inner_graph_builder.parent_catch_ = GetCurrentTryCatchBlock();

  // Set the inner graph builder to build in the current block.
  inner_graph_builder.current_block_ = current_block_;

  ReduceResult result =
      inner_graph_builder.BuildInlined(context, function, new_target, args);
  if (result.IsDoneWithAbort()) {
    DCHECK_NULL(inner_graph_builder.current_block_);
    current_block_ = nullptr;
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "== Finished inlining (abort) " << shared.object()
                << std::endl;
    }
    return ReduceResult::DoneWithAbort();
  }

  // Propagate KnownNodeAspects back to the caller.
  current_interpreter_frame_.set_known_node_aspects(
      inner_graph_builder.current_interpreter_frame_.known_node_aspects());

  if (result.IsDoneWithoutValue()) {
    DCHECK(inner_graph_builder.terminated_with_fused_branch_);
    // If we terminated inlining with a fused branch, we are skipping the
    // creation of a new block.
    // There are 3 possible states:
    //   1) There is already a valid `current_block_` created by the fused
    //   branch fallthrough.
    //   2) The next offset is a merge point, in which case a new block will be
    //   created for the next bytecode automatically in `VisitSingleBytecode()`.
    //   3) The test condition was inlined into the current function and fused
    //   into a branch in a parent of the current function. That means that we
    //   fully processed the current function.
    DCHECK(
        /* (1) */ current_block_ != nullptr ||
        /* (2) */
        (iterator_.next_bytecode() != interpreter::Bytecode::kIllegal &&
         IsOffsetAMergePoint(iterator_.next_offset())) ||
        /* (3) */
        (iterator_.current_bytecode() == interpreter::Bytecode::kReturn &&
         iterator_.next_bytecode() == interpreter::Bytecode::kIllegal));
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "== Finished inlining (fused branch) " << shared.object()
                << std::endl;
    }
    return result;
  }

  DCHECK(result.IsDoneWithValue());
  // Resume execution using the final block of the inner builder.
  current_block_ = inner_graph_builder.current_block_;

  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "== Finished inlining " << shared.object() << std::endl;
  }
  return result;
}

namespace {

bool CanInlineArrayIteratingBuiltin(compiler::JSHeapBroker* broker,
                                    const PossibleMaps& maps,
                                    ElementsKind* kind_return) {
  DCHECK_NE(0, maps.size());
  *kind_return = maps.at(0).elements_kind();
  for (compiler::MapRef map : maps) {
    if (!map.supports_fast_array_iteration(broker) ||
        !UnionElementsKindUptoSize(kind_return, map.elements_kind())) {
      return false;
    }
  }
  return true;
}

}  // namespace

ReduceResult MaglevGraphBuilder::TryReduceArrayForEach(
    compiler::JSFunctionRef target, CallArguments& args) {
  ValueNode* receiver = args.receiver();
  if (!receiver) return ReduceResult::Fail();

  if (args.count() < 1) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  ! Failed to reduce Array.prototype.forEach - not enough "
                   "arguments"
                << std::endl;
    }
    return ReduceResult::Fail();
  }

  auto node_info = known_node_aspects().TryGetInfoFor(receiver);
  if (!node_info || !node_info->possible_maps_are_known()) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  ! Failed to reduce Array.prototype.forEach - receiver "
                   "map is unknown"
                << std::endl;
    }
    return ReduceResult::Fail();
  }

  ElementsKind elements_kind;
  if (!CanInlineArrayIteratingBuiltin(broker(), node_info->possible_maps(),
                                      &elements_kind)) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  ! Failed to reduce Array.prototype.forEach - doesn't "
                   "support fast array iteration or incompatible maps"
                << std::endl;
    }
    return ReduceResult::Fail();
  }

  // TODO(leszeks): May only be needed for holey elements kinds.
  if (!broker()->dependencies()->DependOnNoElementsProtector()) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  ! Failed to reduce Array.prototype.forEach - invalidated "
                   "no elements protector"
                << std::endl;
    }
    return ReduceResult::Fail();
  }

  ValueNode* callback = args[0];
  if (!callback->is_tagged()) {
    if (v8_flags.trace_maglev_graph_building) {
      std::cout << "  ! Failed to reduce Array.prototype.forEach - callback is "
                   "untagged value"
                << std::endl;
    }
    return ReduceResult::Fail();
  }

  ValueNode* this_arg =
      args.count() > 1 ? args[1] : GetRootConstant(RootIndex::kUndefinedValue);

  ValueNode* original_length = BuildLoadJSArrayLength(receiver).value();

  // Elide the callable check if the node is known callable.
  EnsureType(callback, NodeType::kCallable, [&](NodeType old_type) {
    // ThrowIfNotCallable is wrapped in a lazy_deopt_scope to make sure the
    // exception has the right call stack.
    DeoptFrameScope lazy_deopt_scope(
        this, Builtin::kArrayForEachLoopLazyDeoptContinuation, target,
        base::VectorOf<ValueNode*>({receiver, callback, this_arg,
                                    GetSmiConstant(0), original_length}));
    AddNewNode<ThrowIfNotCallable>({callback});
  });

  ValueNode* original_length_int32 = GetInt32(original_length);

  // Remember the receiver map set before entering the loop the call.
  bool receiver_maps_were_unstable = node_info->possible_maps_are_unstable();
  PossibleMaps receiver_maps_before_loop(node_info->possible_maps());
  NodeType receiver_type_before_loop = node_info->type();

  // Create a sub graph builder with one variable (for the index)
  MaglevSubGraphBuilder sub_builder(this, 1);
  MaglevSubGraphBuilder::Variable var_index(0);

  MaglevSubGraphBuilder::Label loop_end(&sub_builder, 1);

  // ```
  // index = 0
  // bind loop_header
  // ```
  sub_builder.set(var_index, GetSmiConstant(0));
  MaglevSubGraphBuilder::LoopLabel loop_header =
      sub_builder.BeginLoop({&var_index});

  // Reset known state that is cleared by BeginLoop, but is known to be true on
  // the first iteration, and will be re-checked at the end of the loop.

  // Reset the known receiver maps if necessary.
  if (receiver_maps_were_unstable) {
    node_info->SetPossibleMaps(receiver_maps_before_loop,
                               receiver_maps_were_unstable,
                               receiver_type_before_loop);
    known_node_aspects().any_map_for_any_node_is_unstable = true;
  } else {
    DCHECK_EQ(node_info->possible_maps().size(),
              receiver_maps_before_loop.size());
  }
  // Reset the cached loaded array length to the original length.
  RecordKnownProperty(receiver, broker()->length_string(), original_length,
                      false, compiler::AccessMode::kLoad);

  // ```
  // if (index_int32 < length_int32)
  //   fallthrough
  // else
  //   goto end
  // ```
  Phi* index_tagged = sub_builder.get(var_index)->Cast<Phi>();
  EnsureType(index_tagged, NodeType::kSmi);
  ValueNode* index_int32 = GetInt32(index_tagged);

  sub_builder.GotoIfFalse<BranchIfInt32Compare>(
      &loop_end, {index_int32, original_length_int32}, Operation::kLessThan);

  // ```
  // next_index = index + 1
  // ```
  ValueNode* next_index_int32 =
      AddNewNode<Int32IncrementWithOverflow>({index_int32});
  EnsureType(next_index_int32, NodeType::kSmi);
  // TODO(leszeks): Assert Smi.

  // ```
  // element = array.elements[index]
  // ```
  ValueNode* elements =
      AddNewNode<LoadTaggedField>({receiver}, JSArray::kElementsOffset);
  ValueNode* element;
  if (IsDoubleElementsKind(elements_kind)) {
    element = AddNewNode<LoadFixedDoubleArrayElement>({elements, index_int32});
  } else {
    element = AddNewNode<LoadFixedArrayElement>({elements, index_int32});
  }

  base::Optional<MaglevSubGraphBuilder::Label> skip_call;
  if (IsHoleyElementsKind(elements_kind)) {
    // ```
    // if (element is hole) goto skip_call
    // ```
    skip_call.emplace(&sub_builder, 2);
    if (elements_kind == HOLEY_DOUBLE_ELEMENTS) {
      sub_builder.GotoIfTrue<BranchIfFloat64IsHole>(&*skip_call, {element});
    } else {
      sub_builder.GotoIfTrue<BranchIfRootConstant>(&*skip_call, {element},
                                                   RootIndex::kTheHoleValue);
    }
  }

  // ```
  // callback(this_arg, element, array)
  // ```
  ReduceResult result;
  {
    DeoptFrameScope lazy_deopt_scope(
        this, Builtin::kArrayForEachLoopLazyDeoptContinuation, target,
        base::VectorOf<ValueNode*>(
            {receiver, callback, this_arg, next_index_int32, original_length}));

    CallArguments call_args =
        args.count() < 2
            ? CallArguments(ConvertReceiverMode::kNullOrUndefined,
                            {element, index_tagged, receiver})
            : CallArguments(ConvertReceiverMode::kAny,
                            {this_arg, element, index_tagged, receiver});

    compiler::FeedbackSource feedback_source = std::exchange(
        current_speculation_feedback_, compiler::FeedbackSource());
    result = ReduceCall(callback, call_args, feedback_source,
                        SpeculationMode::kAllowSpeculation);
    current_speculation_feedback_ = feedback_source;
  }

  // ```
  // index = next_index
  // jump loop_header
  // ```
  DCHECK_IMPLIES(result.IsDoneWithAbort(), current_block_ == nullptr);

  // If any of the receiver's maps were unstable maps, we have to re-check the
  // maps on each iteration, in case the callback changed them. That said, we
  // know that the maps are valid on the first iteration, so we can rotate the
  // check to _after_ the callback, and then elide it if the receiver maps are
  // still known to be valid (i.e. the known maps after the call are contained
  // inside the known maps before the call).
  bool recheck_maps_after_call = receiver_maps_were_unstable;
  if (recheck_maps_after_call) {
    if (current_block_ == nullptr) {
      // No need to recheck maps if this code is unreachable.
      recheck_maps_after_call = false;
    } else {
      // No need to recheck maps if there are known maps...
      if (auto receiver_info_after_call =
              known_node_aspects().TryGetInfoFor(receiver)) {
        // ... and those known maps are equal to, or a subset of, the maps
        // before the call.
        if (receiver_info_after_call &&
            receiver_info_after_call->possible_maps_are_known()) {
          recheck_maps_after_call = receiver_maps_before_loop.contains(
              receiver_info_after_call->possible_maps());
        }
      }
    }
  }

  {
    // Make sure to finish the loop if we eager deopt in the map check or index
    // check.
    DeoptFrameScope eager_deopt_scope(
        this, Builtin::kArrayForEachLoopEagerDeoptContinuation, target,
        base::VectorOf<ValueNode*>(
            {receiver, callback, this_arg, next_index_int32, original_length}));

    if (recheck_maps_after_call) {
      // Build the CheckMap manually, since we're doing it with already known
      // maps rather than feedback, and we don't need to update known node
      // aspects or types since we're at the end of the loop anyway.
      bool emit_check_with_migration = std::any_of(
          receiver_maps_before_loop.begin(), receiver_maps_before_loop.end(),
          [](compiler::MapRef map) { return map.is_migration_target(); });
      if (emit_check_with_migration) {
        AddNewNode<CheckMapsWithMigration>({receiver},
                                           receiver_maps_before_loop,
                                           CheckType::kOmitHeapObjectCheck);
      } else {
        AddNewNode<CheckMaps>({receiver}, receiver_maps_before_loop,
                              CheckType::kOmitHeapObjectCheck);
      }
    }

    // Check if the index is still in bounds, in case the callback changed the
    // length.
    ValueNode* current_length = BuildLoadJSArrayLength(receiver).value();
    // Reference compare the loaded length against the original length. If this
    // is the same value node, then we didn't have any side effects and didn't
    // clear the cached length.
    if (current_length != original_length) {
      AddNewNode<CheckInt32Condition>(
          {original_length_int32, GetInt32(current_length)},
          AssertCondition::kUnsignedLessThanEqual,
          DeoptimizeReason::kArrayLengthChanged);
    }
  }

  if (skip_call.has_value()) {
    sub_builder.Goto(&*skip_call);
    sub_builder.Bind(&*skip_call);
  }

  sub_builder.set(var_index, next_index_int32);
  sub_builder.EndLoop(&loop_header);

  // ```
  // bind end
  // ```
  sub_builder.Bind(&loop_end);

  return GetRootConstant(RootIndex::kUndefinedValue);
}

ReduceResult MaglevGraphBuilder::TryReduceStringFromCharCode(
    compiler::JSFunctionRef target, CallArguments& args) {
  if (args.count() != 1) return ReduceResult::Fail();
  return AddNewNode<BuiltinStringFromCharCode>({GetTruncatedInt32ForToNumber(
      args[0], ToNumberHint::kAssumeNumberOrOddball)});
}

ReduceResult MaglevGraphBuilder::TryReduceStringPrototypeCharCodeAt(
    compiler::JSFunctionRef target, CallArguments& args) {
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());
  ValueNode* index;
  if (args.count() == 0) {
    // Index is the undefined object. ToIntegerOrInfinity(undefined) = 0.
    index = GetInt32Constant(0);
  } else {
    index = GetInt32ElementIndex(args[0]);
  }
  // Any other argument is ignored.

  // Try to constant-fold if receiver and index are constant
  if (auto cst = TryGetConstant(receiver)) {
    if (cst->IsString() && index->Is<Int32Constant>()) {
      compiler::StringRef str = cst->AsString();
      int idx = index->Cast<Int32Constant>()->value();
      if (idx >= 0 && idx < str.length()) {
        if (base::Optional<uint16_t> value = str.GetChar(broker(), idx)) {
          return GetSmiConstant(*value);
        }
      }
    }
  }

  // Ensure that {receiver} is actually a String.
  BuildCheckString(receiver);
  // And index is below length.
  ValueNode* length = AddNewNode<StringLength>({receiver});
  AddNewNode<CheckInt32Condition>({index, length},
                                  AssertCondition::kUnsignedLessThan,
                                  DeoptimizeReason::kOutOfBounds);
  return AddNewNode<BuiltinStringPrototypeCharCodeOrCodePointAt>(
      {receiver, index},
      BuiltinStringPrototypeCharCodeOrCodePointAt::kCharCodeAt);
}

ReduceResult MaglevGraphBuilder::TryReduceStringPrototypeCodePointAt(
    compiler::JSFunctionRef target, CallArguments& args) {
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());
  ValueNode* index;
  if (args.count() == 0) {
    // Index is the undefined object. ToIntegerOrInfinity(undefined) = 0.
    index = GetInt32Constant(0);
  } else {
    index = GetInt32ElementIndex(args[0]);
  }
  // Any other argument is ignored.
  // Ensure that {receiver} is actually a String.
  BuildCheckString(receiver);
  // And index is below length.
  ValueNode* length = AddNewNode<StringLength>({receiver});
  AddNewNode<CheckInt32Condition>({index, length},
                                  AssertCondition::kUnsignedLessThan,
                                  DeoptimizeReason::kOutOfBounds);
  return AddNewNode<BuiltinStringPrototypeCharCodeOrCodePointAt>(
      {receiver, index},
      BuiltinStringPrototypeCharCodeOrCodePointAt::kCodePointAt);
}

template <typename LoadNode>
ReduceResult MaglevGraphBuilder::TryBuildLoadDataView(const CallArguments& args,
                                                      ExternalArrayType type) {
  if (!broker()->dependencies()->DependOnArrayBufferDetachingProtector()) {
    // TODO(victorgomes): Add checks whether the array has been detached.
    return ReduceResult::Fail();
  }
  // TODO(victorgomes): Add data view to known types.
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());
  AddNewNode<CheckInstanceType>({receiver}, CheckType::kCheckHeapObject,
                                JS_DATA_VIEW_TYPE);
  // TODO(v8:11111): Optimize for JS_RAB_GSAB_DATA_VIEW_TYPE too.
  ValueNode* offset =
      args[0] ? GetInt32ElementIndex(args[0]) : GetInt32Constant(0);
  AddNewNode<CheckJSDataViewBounds>({receiver, offset}, type);
  ValueNode* is_little_endian =
      args[1] ? GetTaggedValue(args[1]) : GetBooleanConstant(false);
  return AddNewNode<LoadNode>({receiver, offset, is_little_endian}, type);
}

template <typename StoreNode, typename Function>
ReduceResult MaglevGraphBuilder::TryBuildStoreDataView(
    const CallArguments& args, ExternalArrayType type, Function&& getValue) {
  if (!broker()->dependencies()->DependOnArrayBufferDetachingProtector()) {
    // TODO(victorgomes): Add checks whether the array has been detached.
    return ReduceResult::Fail();
  }
  // TODO(victorgomes): Add data view to known types.
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());
  AddNewNode<CheckInstanceType>({receiver}, CheckType::kCheckHeapObject,
                                JS_DATA_VIEW_TYPE);
  // TODO(v8:11111): Optimize for JS_RAB_GSAB_DATA_VIEW_TYPE too.
  ValueNode* offset =
      args[0] ? GetInt32ElementIndex(args[0]) : GetInt32Constant(0);
  AddNewNode<CheckJSDataViewBounds>({receiver, offset},
                                    ExternalArrayType::kExternalFloat64Array);
  ValueNode* value = getValue(args[1]);
  ValueNode* is_little_endian =
      args[2] ? GetTaggedValue(args[2]) : GetBooleanConstant(false);
  AddNewNode<StoreNode>({receiver, offset, value, is_little_endian}, type);
  return GetRootConstant(RootIndex::kUndefinedValue);
}

ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeGetInt8(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildLoadDataView<LoadSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt8Array);
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeSetInt8(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildStoreDataView<StoreSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt8Array, [&](ValueNode* value) {
        return value ? GetInt32(value) : GetInt32Constant(0);
      });
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeGetInt16(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildLoadDataView<LoadSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt16Array);
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeSetInt16(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildStoreDataView<StoreSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt16Array, [&](ValueNode* value) {
        return value ? GetInt32(value) : GetInt32Constant(0);
      });
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeGetInt32(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildLoadDataView<LoadSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt32Array);
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeSetInt32(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildStoreDataView<StoreSignedIntDataViewElement>(
      args, ExternalArrayType::kExternalInt32Array, [&](ValueNode* value) {
        return value ? GetInt32(value) : GetInt32Constant(0);
      });
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeGetFloat64(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildLoadDataView<LoadDoubleDataViewElement>(
      args, ExternalArrayType::kExternalFloat64Array);
}
ReduceResult MaglevGraphBuilder::TryReduceDataViewPrototypeSetFloat64(
    compiler::JSFunctionRef target, CallArguments& args) {
  return TryBuildStoreDataView<StoreDoubleDataViewElement>(
      args, ExternalArrayType::kExternalFloat64Array, [&](ValueNode* value) {
        return value ? GetHoleyFloat64ForToNumber(
                           value, ToNumberHint::kAssumeNumberOrOddball)
                     : GetFloat64Constant(
                           std::numeric_limits<double>::quiet_NaN());
      });
}

ReduceResult MaglevGraphBuilder::TryReduceFunctionPrototypeCall(
    compiler::JSFunctionRef target, CallArguments& args) {
  // We can't reduce Function#call when there is no receiver function.
  if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
    return ReduceResult::Fail();
  }
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());
  args.PopReceiver(ConvertReceiverMode::kAny);

  compiler::FeedbackSource source = current_speculation_feedback_;
  if (!source.IsValid()) {
    return ReduceCall(receiver, args);
  }
  // Reset speculation feedback source to no feedback.
  current_speculation_feedback_ = compiler::FeedbackSource();
  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForCall(source);
  DCHECK_EQ(processed_feedback.kind(), compiler::ProcessedFeedback::kCall);
  const compiler::CallFeedback& call_feedback = processed_feedback.AsCall();
  return ReduceCall(receiver, args, source, call_feedback.speculation_mode());
}

ReduceResult MaglevGraphBuilder::TryReduceArrayPrototypePush(
    compiler::JSFunctionRef target, CallArguments& args) {
  // We can't reduce Function#call when there is no receiver function.
  if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
    return ReduceResult::Fail();
  }
  if (args.count() != 1) return ReduceResult::Fail();
  ValueNode* receiver = GetTaggedOrUndefined(args.receiver());

  auto node_info = known_node_aspects().TryGetInfoFor(receiver);
  // If the map set is not found, then we don't know anything about the map of
  // the receiver, so bail.
  if (!node_info || !node_info->possible_maps_are_known()) {
    return ReduceResult::Fail();
  }

  // If the set of possible maps is empty, then there's no possible map for this
  // receiver, therefore this path is unreachable at runtime. We're unlikely to
  // ever hit this case, BuildCheckMaps should already unconditionally deopt,
  // but check it in case another checking operation fails to statically
  // unconditionally deopt.
  if (node_info->possible_maps().is_empty()) {
    // TODO(leszeks): Add an unreachable assert here.
    return ReduceResult::DoneWithAbort();
  }

  if (!broker()->dependencies()->DependOnNoElementsProtector()) {
    return ReduceResult::Fail();
  }

  ElementsKind kind;
  ZoneVector<compiler::MapRef> receiver_map_refs(zone());
  // Check that all receiver maps are JSArray maps with compatible elements
  // kinds.
  for (compiler::MapRef map : node_info->possible_maps()) {
    if (!map.IsJSArrayMap()) return ReduceResult::Fail();
    ElementsKind packed = GetPackedElementsKind(map.elements_kind());
    if (!IsFastElementsKind(packed)) return ReduceResult::Fail();
    if (!map.supports_fast_array_resize(broker())) {
      return ReduceResult::Fail();
    }
    if (receiver_map_refs.empty()) {
      kind = packed;
    } else if (kind != packed) {
      return ReduceResult::Fail();
    }
    receiver_map_refs.push_back(map);
  }

  ValueNode* value = ConvertForStoring(args[0], kind);

  ValueNode* old_array_length_smi =
      AddNewNode<LoadTaggedField>({receiver}, JSArray::kLengthOffset);
  ValueNode* old_array_length =
      AddNewNode<UnsafeSmiUntag>({old_array_length_smi});
  ValueNode* new_array_length =
      AddNewNode<Int32IncrementWithOverflow>({old_array_length});
  ValueNode* new_array_length_smi = GetSmiValue(new_array_length);

  ValueNode* elements_array =
      AddNewNode<LoadTaggedField>({receiver}, JSObject::kElementsOffset);

  ValueNode* elements_array_length =
      AddNewNode<UnsafeSmiUntag>({AddNewNode<LoadTaggedField>(
          {elements_array}, FixedArray::kLengthOffset)});

  elements_array = AddNewNode<MaybeGrowAndEnsureWritableFastElements>(
      {elements_array, receiver, old_array_length, elements_array_length},
      kind);

  AddNewNode<StoreTaggedFieldNoWriteBarrier>({receiver, new_array_length_smi},
                                             JSArray::kLengthOffset);

  // Do the store
  if (IsDoubleElementsKind(kind)) {
    AddNewNode<StoreFixedDoubleArrayElement>(
        {elements_array, old_array_length, value});
  } else {
    BuildStoreFixedArrayElement(elements_array, old_array_length, value);
  }

  SetAccumulator(new_array_length);
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::TryReduceFunctionPrototypeHasInstance(
    compiler::JSFunctionRef target, CallArguments& args) {
  // We can't reduce Function#hasInstance when there is no receiver function.
  if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
    return ReduceResult::Fail();
  }
  if (args.count() != 1) {
    return ReduceResult::Fail();
  }
  compiler::OptionalHeapObjectRef maybe_receiver_constant =
      TryGetConstant(args.receiver());
  if (!maybe_receiver_constant) {
    return ReduceResult::Fail();
  }
  compiler::HeapObjectRef receiver_object = maybe_receiver_constant.value();
  if (!receiver_object.IsJSObject() ||
      !receiver_object.map(broker()).is_callable()) {
    return ReduceResult::Fail();
  }
  return BuildOrdinaryHasInstance(GetTaggedValue(args[0]),
                                  receiver_object.AsJSObject(), nullptr);
}

ReduceResult MaglevGraphBuilder::TryReduceObjectPrototypeHasOwnProperty(
    compiler::JSFunctionRef target, CallArguments& args) {
  if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
    return ReduceResult::Fail();
  }
  if (args.receiver() != current_for_in_state.receiver) {
    return ReduceResult::Fail();
  }
  if (args.count() != 1 || args[0] != current_for_in_state.key) {
    return ReduceResult::Fail();
  }
  return GetRootConstant(RootIndex::kTrueValue);
}

ReduceResult MaglevGraphBuilder::TryReduceMathRound(
    compiler::JSFunctionRef target, CallArguments& args) {
  return DoTryReduceMathRound(args, Float64Round::Kind::kNearest);
}

ReduceResult MaglevGraphBuilder::TryReduceMathFloor(
    compiler::JSFunctionRef target, CallArguments& args) {
  return DoTryReduceMathRound(args, Float64Round::Kind::kFloor);
}

ReduceResult MaglevGraphBuilder::TryReduceMathCeil(
    compiler::JSFunctionRef target, CallArguments& args) {
  return DoTryReduceMathRound(args, Float64Round::Kind::kCeil);
}

ReduceResult MaglevGraphBuilder::DoTryReduceMathRound(CallArguments& args,
                                                      Float64Round::Kind kind) {
  if (args.count() == 0) {
    return GetRootConstant(RootIndex::kNanValue);
  }
  ValueNode* arg = args[0];
  switch (arg->value_representation()) {
    case ValueRepresentation::kInt32:
    case ValueRepresentation::kUint32:
      return arg;
    case ValueRepresentation::kTagged:
      if (CheckType(arg, NodeType::kSmi)) return arg;
      if (CheckType(arg, NodeType::kNumberOrOddball)) {
        arg = GetHoleyFloat64ForToNumber(arg,
                                         ToNumberHint::kAssumeNumberOrOddball);
      } else {
        DeoptFrameScope continuation_scope(this,
                                           Float64Round::continuation(kind));
        ToNumberOrNumeric* conversion =
            AddNewNode<ToNumberOrNumeric>({arg}, Object::Conversion::kToNumber);
        arg = AddNewNode<UncheckedNumberOrOddballToFloat64>(
            {conversion}, TaggedToFloat64ConversionType::kOnlyNumber);
      }
      break;
    case ValueRepresentation::kWord64:
      UNREACHABLE();
    case ValueRepresentation::kFloat64:
    case ValueRepresentation::kHoleyFloat64:
      break;
  }
  if (IsSupported(CpuOperation::kFloat64Round)) {
    return AddNewNode<Float64Round>({arg}, kind);
  }

  // Update the first argument, in case there was a side-effecting ToNumber
  // conversion.
  args.set_arg(0, arg);
  return ReduceResult::Fail();
}

ReduceResult MaglevGraphBuilder::TryReduceStringConstructor(
    compiler::JSFunctionRef target, CallArguments& args) {
  if (args.count() == 0) {
    return GetRootConstant(RootIndex::kempty_string);
  }

  return BuildToString(args[0], ToString::kConvertSymbol);
}

ReduceResult MaglevGraphBuilder::TryReduceMathPow(
    compiler::JSFunctionRef target, CallArguments& args) {
  if (args.count() < 2) {
    // For < 2 args, we'll be calculating Math.Pow(arg[0], undefined), which is
    // ToNumber(arg[0]) ** NaN == NaN. So we can just return NaN.
    // However, if there is a single argument and it's tagged, we have to call
    // ToNumber on it before returning NaN, for side effects. This call could
    // lazy deopt, which would mean we'd need a continuation to actually set
    // the NaN return value... it's easier to just bail out, this should be
    // an uncommon case anyway.
    if (args.count() == 1 && args[0]->properties().is_tagged()) {
      return ReduceResult::Fail();
    }
    return GetRootConstant(RootIndex::kNanValue);
  }
  // If both arguments are tagged, it is cheaper to call Math.Pow builtin,
  // instead of Float64Exponentiate, since we are still making a call and we
  // don't need to unbox both inputs. See https://crbug.com/1393643.
  if (args[0]->properties().is_tagged() && args[1]->properties().is_tagged()) {
    // The Math.pow call will be created in CallKnownJSFunction reduction.
    return ReduceResult::Fail();
  }
  ValueNode* left =
      GetHoleyFloat64ForToNumber(args[0], ToNumberHint::kAssumeNumber);
  ValueNode* right =
      GetHoleyFloat64ForToNumber(args[1], ToNumberHint::kAssumeNumber);
  return AddNewNode<Float64Exponentiate>({left, right});
}

#define MAP_MATH_UNARY_TO_IEEE_754(V) \
  V(MathAcos, acos)                   \
  V(MathAcosh, acosh)                 \
  V(MathAsin, asin)                   \
  V(MathAsinh, asinh)                 \
  V(MathAtan, atan)                   \
  V(MathAtanh, atanh)                 \
  V(MathCbrt, cbrt)                   \
  V(MathCos, cos)                     \
  V(MathCosh, cosh)                   \
  V(MathExp, exp)                     \
  V(MathExpm1, expm1)                 \
  V(MathLog, log)                     \
  V(MathLog1p, log1p)                 \
  V(MathLog10, log10)                 \
  V(MathLog2, log2)                   \
  V(MathSin, sin)                     \
  V(MathSinh, sinh)                   \
  V(MathTan, tan)                     \
  V(MathTanh, tanh)

#define MATH_UNARY_IEEE_BUILTIN_REDUCER(Name, IeeeOp)                \
  ReduceResult MaglevGraphBuilder::TryReduce##Name(                  \
      compiler::JSFunctionRef target, CallArguments& args) {         \
    if (args.count() < 1) {                                          \
      return GetRootConstant(RootIndex::kNanValue);                  \
    }                                                                \
    ValueNode* value =                                               \
        GetFloat64ForToNumber(args[0], ToNumberHint::kAssumeNumber); \
    return AddNewNode<Float64Ieee754Unary>(                          \
        {value}, ExternalReference::ieee754_##IeeeOp##_function());  \
  }

MAP_MATH_UNARY_TO_IEEE_754(MATH_UNARY_IEEE_BUILTIN_REDUCER)

#undef MATH_UNARY_IEEE_BUILTIN_REDUCER
#undef MAP_MATH_UNARY_TO_IEEE_754

ReduceResult MaglevGraphBuilder::TryReduceBuiltin(
    compiler::JSFunctionRef target, compiler::SharedFunctionInfoRef shared,
    CallArguments& args, const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  if (args.mode() != CallArguments::kDefault) {
    // TODO(victorgomes): Maybe inline the spread stub? Or call known function
    // directly if arguments list is an array.
    return ReduceResult::Fail();
  }
  if (feedback_source.IsValid() &&
      speculation_mode == SpeculationMode::kDisallowSpeculation) {
    // TODO(leszeks): Some builtins might be inlinable without speculation.
    return ReduceResult::Fail();
  }
  CallSpeculationScope speculate(this, feedback_source);
  if (!shared.HasBuiltinId()) {
    return ReduceResult::Fail();
  }
  if (v8_flags.trace_maglev_graph_building) {
    std::cout << "  ! Trying to reduce builtin "
              << Builtins::name(shared.builtin_id()) << std::endl;
  }
  switch (shared.builtin_id()) {
#define CASE(Name)       \
  case Builtin::k##Name: \
    return TryReduce##Name(target, args);
    MAGLEV_REDUCED_BUILTIN(CASE)
#undef CASE
    default:
      // TODO(v8:7700): Inline more builtins.
      return ReduceResult::Fail();
  }
}

ValueNode* MaglevGraphBuilder::GetConvertReceiver(
    compiler::SharedFunctionInfoRef shared, const CallArguments& args) {
  return GetTaggedValue(GetRawConvertReceiver(shared, args));
}

ValueNode* MaglevGraphBuilder::GetRawConvertReceiver(
    compiler::SharedFunctionInfoRef shared, const CallArguments& args) {
  if (shared.native() || shared.language_mode() == LanguageMode::kStrict) {
    if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
      return GetRootConstant(RootIndex::kUndefinedValue);
    } else {
      return args.receiver();
    }
  }
  if (args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined) {
    return GetConstant(
        broker()->target_native_context().global_proxy_object(broker()));
  }
  ValueNode* receiver = args.receiver();
  if (CheckType(receiver, NodeType::kJSReceiver)) return receiver;
  if (compiler::OptionalHeapObjectRef maybe_constant =
          TryGetConstant(receiver)) {
    compiler::HeapObjectRef constant = maybe_constant.value();
    if (constant.IsNullOrUndefined()) {
      return GetConstant(
          broker()->target_native_context().global_proxy_object(broker()));
    }
  }
  return AddNewNode<ConvertReceiver>({GetTaggedValue(receiver)},
                                     broker()->target_native_context(),
                                     args.receiver_mode());
}

template <typename CallNode, typename... Args>
CallNode* MaglevGraphBuilder::AddNewCallNode(const CallArguments& args,
                                             Args&&... extra_args) {
  size_t input_count = args.count_with_receiver() + CallNode::kFixedInputCount;
  return AddNewNode<CallNode>(
      input_count,
      [&](CallNode* call) {
        int arg_index = 0;
        call->set_arg(arg_index++, GetTaggedOrUndefined(args.receiver()));
        for (size_t i = 0; i < args.count(); ++i) {
          call->set_arg(arg_index++, GetTaggedValue(args[i]));
        }
      },
      std::forward<Args>(extra_args)...);
}

ValueNode* MaglevGraphBuilder::BuildGenericCall(ValueNode* target,
                                                Call::TargetType target_type,
                                                const CallArguments& args) {
  // TODO(victorgomes): We do not collect call feedback from optimized/inlined
  // calls. In order to be consistent, we don't pass the feedback_source to the
  // IR, so that we avoid collecting for generic calls as well. We might want to
  // revisit this in the future.
  switch (args.mode()) {
    case CallArguments::kDefault:
      return AddNewCallNode<Call>(args, args.receiver_mode(), target_type,
                                  target, GetContext());
    case CallArguments::kWithSpread:
      DCHECK_EQ(args.receiver_mode(), ConvertReceiverMode::kAny);
      return AddNewCallNode<CallWithSpread>(args, target, GetContext());
    case CallArguments::kWithArrayLike:
      DCHECK_EQ(args.receiver_mode(), ConvertReceiverMode::kAny);
      return AddNewNode<CallWithArrayLike>(
          {target, args.receiver(), args[0], GetContext()});
  }
}

ValueNode* MaglevGraphBuilder::BuildCallSelf(
    ValueNode* context, ValueNode* function, ValueNode* new_target,
    compiler::SharedFunctionInfoRef shared, CallArguments& args) {
  ValueNode* receiver = GetConvertReceiver(shared, args);
  size_t input_count = args.count() + CallSelf::kFixedInputCount;
  graph()->set_has_recursive_calls(true);
  return AddNewNode<CallSelf>(
      input_count,
      [&](CallSelf* call) {
        for (int i = 0; i < static_cast<int>(args.count()); i++) {
          call->set_arg(i, GetTaggedValue(args[i]));
        }
      },
      shared, function, context, receiver, new_target);
}

bool MaglevGraphBuilder::TargetIsCurrentCompilingUnit(
    compiler::JSFunctionRef target) {
  if (compilation_unit_->info()->specialize_to_function_context()) {
    return target.object().equals(
        compilation_unit_->info()->toplevel_function());
  }
  return target.object()->shared() ==
         compilation_unit_->info()->toplevel_function()->shared();
}

ReduceResult MaglevGraphBuilder::ReduceCallForApiFunction(
    compiler::FunctionTemplateInfoRef api_callback,
    compiler::OptionalSharedFunctionInfoRef maybe_shared,
    compiler::OptionalJSObjectRef api_holder, CallArguments& args) {
  if (args.mode() != CallArguments::kDefault) {
    // TODO(victorgomes): Maybe inline the spread stub? Or call known function
    // directly if arguments list is an array.
    return ReduceResult::Fail();
  }
  compiler::OptionalCallHandlerInfoRef maybe_call_handler_info =
      api_callback.call_code(broker());
  if (!maybe_call_handler_info.has_value()) {
    // TODO(ishell): distinguish TryMakeRef-related failure from empty function
    // case and generate "return undefined" for the latter.
    return ReduceResult::Fail();
  }
  compiler::CallHandlerInfoRef call_handler_info =
      maybe_call_handler_info.value();
  compiler::ObjectRef data = call_handler_info.data(broker());

  size_t input_count = args.count() + CallKnownApiFunction::kFixedInputCount;
  ValueNode* receiver;
  if (maybe_shared.has_value()) {
    receiver = GetConvertReceiver(maybe_shared.value(), args);
  } else {
    receiver = args.receiver();
    CHECK_NOT_NULL(receiver);
  }

  CallKnownApiFunction::Mode mode =
      broker()->dependencies()->DependOnNoProfilingProtector()
          ? CallKnownApiFunction::kNoProfiling
          : CallKnownApiFunction::kGeneric;

  return AddNewNode<CallKnownApiFunction>(
      input_count,
      [&](CallKnownApiFunction* call) {
        for (int i = 0; i < static_cast<int>(args.count()); i++) {
          call->set_arg(i, GetTaggedValue(args[i]));
        }
      },
      mode, api_callback, call_handler_info, data, api_holder, GetContext(),
      receiver);
}

ReduceResult MaglevGraphBuilder::TryBuildCallKnownApiFunction(
    compiler::JSFunctionRef function, compiler::SharedFunctionInfoRef shared,
    CallArguments& args) {
  compiler::OptionalFunctionTemplateInfoRef maybe_function_template_info =
      shared.function_template_info(broker());
  if (!maybe_function_template_info.has_value()) {
    // Not an Api function.
    return ReduceResult::Fail();
  }

  // See if we can optimize this API call.
  compiler::FunctionTemplateInfoRef function_template_info =
      maybe_function_template_info.value();

  compiler::HolderLookupResult api_holder;
  if (function_template_info.accept_any_receiver() &&
      function_template_info.is_signature_undefined(broker())) {
    // We might be able to optimize the API call depending on the
    // {function_template_info}.
    // If the API function accepts any kind of {receiver}, we only need to
    // ensure that the {receiver} is actually a JSReceiver at this point,
    // and also pass that as the {holder}. There are two independent bits
    // here:
    //
    //  a. When the "accept any receiver" bit is set, it means we don't
    //     need to perform access checks, even if the {receiver}'s map
    //     has the "needs access check" bit set.
    //  b. When the {function_template_info} has no signature, we don't
    //     need to do the compatible receiver check, since all receivers
    //     are considered compatible at that point, and the {receiver}
    //     will be pass as the {holder}.

    api_holder =
        compiler::HolderLookupResult{CallOptimization::kHolderIsReceiver};
  } else {
    // Try to infer API holder from the known aspects of the {receiver}.
    api_holder =
        TryInferApiHolderValue(function_template_info, args.receiver());
  }

  switch (api_holder.lookup) {
    case CallOptimization::kHolderIsReceiver:
    case CallOptimization::kHolderFound: {
      // Add lazy deopt point to make the API function appear in exception
      // stack trace.
      base::Optional<DeoptFrameScope> lazy_deopt_scope;
      if (v8_flags.experimental_stack_trace_frames) {
        // The receiver is only necessary for displaying "class.method" name
        // if applicable, so full GetConvertReceiver() is not needed here.
        // Another reason to not use GetConvertReceiver() here because
        // ReduceCallForApiFunction might bailout and we'll end up with
        // unused ConvertReceiver node.
        ValueNode* receiver =
            args.receiver_mode() == ConvertReceiverMode::kNullOrUndefined
                ? GetRootConstant(RootIndex::kUndefinedValue)
                : args.receiver();

        lazy_deopt_scope.emplace(this, Builtin::kGenericLazyDeoptContinuation,
                                 function,
                                 base::VectorOf<ValueNode*>({receiver}));
      }
      return ReduceCallForApiFunction(function_template_info, shared,
                                      api_holder.holder, args);
    }
    case CallOptimization::kHolderNotFound:
      break;
  }

  // We don't have enough information to eliminate the access check
  // and/or the compatible receiver check, so use the generic builtin
  // that does those checks dynamically. This is still significantly
  // faster than the generic call sequence.
  Builtin builtin_name;
  // TODO(ishell): create no-profiling versions of kCallFunctionTemplate
  // builtins and use them here based on DependOnNoProfilingProtector()
  // dependency state.
  if (function_template_info.accept_any_receiver()) {
    DCHECK(!function_template_info.is_signature_undefined(broker()));
    builtin_name = Builtin::kCallFunctionTemplate_CheckCompatibleReceiver;
  } else if (function_template_info.is_signature_undefined(broker())) {
    builtin_name = Builtin::kCallFunctionTemplate_CheckAccess;
  } else {
    builtin_name =
        Builtin::kCallFunctionTemplate_CheckAccessAndCompatibleReceiver;
  }

  // The CallFunctionTemplate builtin requires the {receiver} to be
  // an actual JSReceiver, so make sure we do the proper conversion
  // first if necessary.
  ValueNode* receiver = GetConvertReceiver(shared, args);
  int kContext = 1;
  int kFunctionTemplateInfo = 1;
  int kArgc = 1;
  return AddNewNode<CallBuiltin>(
      kFunctionTemplateInfo + kArgc + kContext + args.count_with_receiver(),
      [&](CallBuiltin* call_builtin) {
        int arg_index = 0;
        call_builtin->set_arg(arg_index++, GetConstant(function_template_info));
        call_builtin->set_arg(
            arg_index++,
            GetInt32Constant(JSParameterCount(static_cast<int>(args.count()))));

        call_builtin->set_arg(arg_index++, receiver);
        for (int i = 0; i < static_cast<int>(args.count()); i++) {
          call_builtin->set_arg(arg_index++, GetTaggedValue(args[i]));
        }
      },
      builtin_name, GetContext());
}

ReduceResult MaglevGraphBuilder::TryBuildCallKnownJSFunction(
    compiler::JSFunctionRef function, ValueNode* new_target,
    CallArguments& args, const compiler::FeedbackSource& feedback_source) {
  // Don't inline CallFunction stub across native contexts.
  if (function.native_context(broker()) != broker()->target_native_context()) {
    return ReduceResult::Fail();
  }
  compiler::SharedFunctionInfoRef shared = function.shared(broker());
  RETURN_IF_DONE(TryBuildCallKnownApiFunction(function, shared, args));

  ValueNode* closure = GetConstant(function);
  ValueNode* context = GetConstant(function.context(broker()));
  if (MaglevIsTopTier() && TargetIsCurrentCompilingUnit(function) &&
      !graph_->is_osr()) {
    return BuildCallSelf(context, closure, new_target, shared, args);
  }
  return TryBuildCallKnownJSFunction(context, closure, new_target, shared,
                                     function.feedback_vector(broker()), args,
                                     feedback_source);
}

ReduceResult MaglevGraphBuilder::TryBuildCallKnownJSFunction(
    ValueNode* context, ValueNode* function, ValueNode* new_target,
    compiler::SharedFunctionInfoRef shared,
    compiler::OptionalFeedbackVectorRef feedback_vector, CallArguments& args,
    const compiler::FeedbackSource& feedback_source) {
  if (v8_flags.maglev_inlining) {
    RETURN_IF_DONE(TryBuildInlinedCall(context, function, new_target, shared,
                                       feedback_vector, args, feedback_source));
  }
  ValueNode* receiver = GetConvertReceiver(shared, args);
  size_t input_count = args.count() + CallKnownJSFunction::kFixedInputCount;
  return AddNewNode<CallKnownJSFunction>(
      input_count,
      [&](CallKnownJSFunction* call) {
        for (int i = 0; i < static_cast<int>(args.count()); i++) {
          call->set_arg(i, GetTaggedValue(args[i]));
        }
      },
      shared, function, context, receiver, new_target);
}

ReduceResult MaglevGraphBuilder::BuildCheckValue(ValueNode* node,
                                                 compiler::HeapObjectRef ref) {
  DCHECK(!ref.IsSmi());
  DCHECK(!ref.IsHeapNumber());

  if (compiler::OptionalHeapObjectRef maybe_constant = TryGetConstant(node)) {
    if (maybe_constant.value().equals(ref)) {
      return ReduceResult::Done();
    }
    return EmitUnconditionalDeopt(DeoptimizeReason::kUnknown);
  }
  if (ref.IsString()) {
    DCHECK(ref.IsInternalizedString());
    AddNewNode<CheckValueEqualsString>({node}, ref.AsInternalizedString());
  } else {
    AddNewNode<CheckValue>({node}, ref);
  }
  SetKnownValue(node, ref);
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::BuildCheckValue(ValueNode* node,
                                                 compiler::ObjectRef ref) {
  if (ref.IsHeapObject() && !ref.IsHeapNumber()) {
    return BuildCheckValue(node, ref.AsHeapObject());
  }
  if (ref.IsSmi()) {
    int ref_value = ref.AsSmi();
    if (IsConstantNode(node->opcode())) {
      if (node->Is<SmiConstant>() &&
          node->Cast<SmiConstant>()->value().value() == ref_value) {
        return ReduceResult::Done();
      }
      if (node->Is<Int32Constant>() &&
          node->Cast<Int32Constant>()->value() == ref_value) {
        return ReduceResult::Done();
      }
      return EmitUnconditionalDeopt(DeoptimizeReason::kUnknown);
    }
    AddNewNode<CheckValueEqualsInt32>({GetInt32(node)}, ref_value);
  } else {
    DCHECK(ref.IsHeapNumber());
    double ref_value = ref.AsHeapNumber().value();
    if (node->Is<Float64Constant>()) {
      if (node->Cast<Float64Constant>()->value().get_scalar() == ref_value) {
        return ReduceResult::Done();
      }
      // TODO(verwaest): Handle NaN.
      return EmitUnconditionalDeopt(DeoptimizeReason::kUnknown);
    } else if (compiler::OptionalHeapObjectRef constant =
                   TryGetConstant(node)) {
      if (constant.value().IsHeapNumber() &&
          constant.value().AsHeapNumber().value() == ref_value) {
        return ReduceResult::Done();
      }
      // TODO(verwaest): Handle NaN.
      return EmitUnconditionalDeopt(DeoptimizeReason::kUnknown);
    }
    AddNewNode<CheckValueEqualsFloat64>({GetFloat64(node)}, ref_value);
  }
  SetKnownValue(node, ref);
  return ReduceResult::Done();
}

ReduceResult MaglevGraphBuilder::ReduceCallForConstant(
    compiler::JSFunctionRef target, CallArguments& args,
    const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  if (args.mode() != CallArguments::kDefault) {
    // TODO(victorgomes): Maybe inline the spread stub? Or call known function
    // directly if arguments list is an array.
    return ReduceResult::Fail();
  }
  compiler::SharedFunctionInfoRef shared = target.shared(broker());
  ValueNode* target_node = GetConstant(target);
  // Do not reduce calls to functions with break points.
  if (!shared.HasBreakInfo(broker())) {
    if (IsClassConstructor(shared.kind())) {
      // If we have a class constructor, we should raise an exception.
      return BuildCallRuntime(Runtime::kThrowConstructorNonCallableError,
                              {target_node});
    }
    DCHECK(IsCallable(*target.object()));
    RETURN_IF_DONE(TryReduceBuiltin(target, shared, args, feedback_source,
                                    speculation_mode));
    RETURN_IF_DONE(TryBuildCallKnownJSFunction(
        target, GetRootConstant(RootIndex::kUndefinedValue), args,
        feedback_source));
  }
  return BuildGenericCall(target_node, Call::TargetType::kJSFunction, args);
}

compiler::HolderLookupResult MaglevGraphBuilder::TryInferApiHolderValue(
    compiler::FunctionTemplateInfoRef function_template_info,
    ValueNode* receiver) {
  const compiler::HolderLookupResult not_found;

  auto receiver_info = known_node_aspects().TryGetInfoFor(receiver);
  if (!receiver_info || !receiver_info->possible_maps_are_known()) {
    // No info about receiver, can't infer API holder.
    return not_found;
  }
  DCHECK(!receiver_info->possible_maps().is_empty());
  compiler::MapRef first_receiver_map = receiver_info->possible_maps()[0];

  // See if we can constant-fold the compatible receiver checks.
  compiler::HolderLookupResult api_holder =
      function_template_info.LookupHolderOfExpectedType(broker(),
                                                        first_receiver_map);
  if (api_holder.lookup == CallOptimization::kHolderNotFound) {
    // Can't infer API holder.
    return not_found;
  }

  // Check that all {receiver_maps} are actually JSReceiver maps and
  // that the {function_template_info} accepts them without access
  // checks (even if "access check needed" is set for {receiver}).
  //
  // API holder might be a receivers's hidden prototype (i.e. the receiver is
  // a global proxy), so in this case the map check or stability dependency on
  // the receiver guard us from detaching a global object from global proxy.
  CHECK(first_receiver_map.IsJSReceiverMap());
  CHECK(!first_receiver_map.is_access_check_needed() ||
        function_template_info.accept_any_receiver());

  for (compiler::MapRef receiver_map : receiver_info->possible_maps()) {
    compiler::HolderLookupResult holder_i =
        function_template_info.LookupHolderOfExpectedType(broker(),
                                                          receiver_map);

    if (api_holder.lookup != holder_i.lookup) {
      // Different API holders, dynamic lookup is required.
      return not_found;
    }
    DCHECK(holder_i.lookup == CallOptimization::kHolderFound ||
           holder_i.lookup == CallOptimization::kHolderIsReceiver);
    if (holder_i.lookup == CallOptimization::kHolderFound) {
      DCHECK(api_holder.holder.has_value() && holder_i.holder.has_value());
      if (!api_holder.holder->equals(*holder_i.holder)) {
        // Different API holders, dynamic lookup is required.
        return not_found;
      }
    }

    CHECK(receiver_map.IsJSReceiverMap());
    CHECK(!receiver_map.is_access_check_needed() ||
          function_template_info.accept_any_receiver());
  }
  return api_holder;
}

ReduceResult MaglevGraphBuilder::ReduceCallForTarget(
    ValueNode* target_node, compiler::JSFunctionRef target, CallArguments& args,
    const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  RETURN_IF_ABORT(BuildCheckValue(target_node, target));
  return ReduceCallForConstant(target, args, feedback_source, speculation_mode);
}

ReduceResult MaglevGraphBuilder::ReduceCallForNewClosure(
    ValueNode* target_node, ValueNode* target_context,
    compiler::SharedFunctionInfoRef shared,
    compiler::OptionalFeedbackVectorRef feedback_vector, CallArguments& args,
    const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  // Do not reduce calls to functions with break points.
  if (args.mode() != CallArguments::kDefault) {
    // TODO(victorgomes): Maybe inline the spread stub? Or call known function
    // directly if arguments list is an array.
    return ReduceResult::Fail();
  }
  if (!shared.HasBreakInfo(broker())) {
    if (IsClassConstructor(shared.kind())) {
      // If we have a class constructor, we should raise an exception.
      return BuildCallRuntime(Runtime::kThrowConstructorNonCallableError,
                              {target_node});
    }
    RETURN_IF_DONE(TryBuildCallKnownJSFunction(
        target_context, target_node,
        GetRootConstant(RootIndex::kUndefinedValue), shared, feedback_vector,
        args, feedback_source));
  }
  return BuildGenericCall(target_node, Call::TargetType::kJSFunction, args);
}

ReduceResult MaglevGraphBuilder::ReduceFunctionPrototypeApplyCallWithReceiver(
    ValueNode* target_node, compiler::JSFunctionRef receiver,
    CallArguments& args, const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  // TODO(victorgomes): This has currently pretty limited usage and does not
  // work with inlining. We need to implement arguments forwarding like TF
  // ReduceCallOrConstructWithArrayLikeOrSpreadOfCreateArguments.
  return ReduceResult::Fail();
  // compiler::NativeContextRef native_context =
  // broker()->target_native_context(); RETURN_IF_ABORT(BuildCheckValue(
  //     target_node, native_context.function_prototype_apply(broker())));
  // ValueNode* receiver_node = GetTaggedOrUndefined(args.receiver());
  // RETURN_IF_ABORT(BuildCheckValue(receiver_node, receiver));
  // if (args.mode() == CallArguments::kDefault) {
  //   if (args.count() == 0) {
  //     // No need for spread.
  //     CallArguments empty_args(ConvertReceiverMode::kNullOrUndefined);
  //     return ReduceCall(receiver, empty_args, feedback_source,
  //                       speculation_mode);
  //   }
  //   if (args.count() == 1 || IsNullValue(args[1]) ||
  //       IsUndefinedValue(args[1])) {
  //     // No need for spread. We have only the new receiver.
  //     CallArguments new_args(ConvertReceiverMode::kAny,
  //                            {GetTaggedValue(args[0])});
  //     return ReduceCall(receiver, new_args, feedback_source,
  //     speculation_mode);
  //   }
  //   if (args.count() > 2) {
  //     // FunctionPrototypeApply only consider two arguments: the new receiver
  //     // and an array-like arguments_list. All others shall be ignored.
  //     args.Truncate(2);
  //   }
  // }
  // return ReduceCall(native_context.function_prototype_apply(broker()), args,
  //                   feedback_source, speculation_mode);
}

void MaglevGraphBuilder::BuildCallWithFeedback(
    ValueNode* target_node, CallArguments& args,
    const compiler::FeedbackSource& feedback_source) {
  const compiler::ProcessedFeedback& processed_feedback =
      broker()->GetFeedbackForCall(feedback_source);
  if (processed_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForCall));
  }

  DCHECK_EQ(processed_feedback.kind(), compiler::ProcessedFeedback::kCall);
  const compiler::CallFeedback& call_feedback = processed_feedback.AsCall();

  if (call_feedback.target().has_value() &&
      call_feedback.target()->IsJSFunction()) {
    CallFeedbackContent content = call_feedback.call_feedback_content();
    compiler::JSFunctionRef feedback_target =
        call_feedback.target()->AsJSFunction();
    if (content == CallFeedbackContent::kReceiver) {
      // TODO(verwaest): Actually implement
      // FunctionPrototypeApplyCallWithReceiver.
      compiler::NativeContextRef native_context =
          broker()->target_native_context();
      feedback_target = native_context.function_prototype_apply(broker());
    } else {
      DCHECK_EQ(CallFeedbackContent::kTarget, content);
    }
    RETURN_VOID_IF_ABORT(BuildCheckValue(target_node, feedback_target));
  }

  PROCESS_AND_RETURN_IF_DONE(ReduceCall(target_node, args, feedback_source,
                                        call_feedback.speculation_mode()),
                             SetAccumulator);
}

ReduceResult MaglevGraphBuilder::ReduceCall(
    ValueNode* target_node, CallArguments& args,
    const compiler::FeedbackSource& feedback_source,
    SpeculationMode speculation_mode) {
  if (compiler::OptionalHeapObjectRef maybe_constant =
          TryGetConstant(target_node)) {
    if (maybe_constant->IsJSFunction()) {
      ReduceResult result =
          ReduceCallForTarget(target_node, maybe_constant->AsJSFunction(), args,
                              feedback_source, speculation_mode);
      RETURN_IF_DONE(result);
    }
  }

  // If the implementation here becomes more complex, we could probably
  // deduplicate the code for FastCreateClosure and CreateClosure by using
  // templates or giving them a shared base class.
  if (FastCreateClosure* create_closure =
          target_node->TryCast<FastCreateClosure>()) {
    ReduceResult result = ReduceCallForNewClosure(
        create_closure, create_closure->context().node(),
        create_closure->shared_function_info(),
        create_closure->feedback_cell().feedback_vector(broker()), args,
        feedback_source, speculation_mode);
    RETURN_IF_DONE(result);
  } else if (CreateClosure* create_closure =
                 target_node->TryCast<CreateClosure>()) {
    ReduceResult result = ReduceCallForNewClosure(
        create_closure, create_closure->context().node(),
        create_closure->shared_function_info(),
        create_closure->feedback_cell().feedback_vector(broker()), args,
        feedback_source, speculation_mode);
    RETURN_IF_DONE(result);
  }

  // On fallthrough, create a generic call.
  return BuildGenericCall(target_node, Call::TargetType::kAny, args);
}

void MaglevGraphBuilder::BuildCallFromRegisterList(
    ConvertReceiverMode receiver_mode) {
  ValueNode* target = LoadRegisterTagged(0);
  interpreter::RegisterList reg_list = iterator_.GetRegisterListOperand(1);
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source(feedback(), slot);
  CallArguments args(receiver_mode, reg_list, current_interpreter_frame_);
  BuildCallWithFeedback(target, args, feedback_source);
}

void MaglevGraphBuilder::BuildCallFromRegisters(
    int arg_count, ConvertReceiverMode receiver_mode) {
  ValueNode* target = LoadRegisterTagged(0);
  const int receiver_count =
      (receiver_mode == ConvertReceiverMode::kNullOrUndefined) ? 0 : 1;
  const int reg_count = arg_count + receiver_count;
  FeedbackSlot slot = GetSlotOperand(reg_count + 1);
  compiler::FeedbackSource feedback_source(feedback(), slot);
  switch (reg_count) {
    case 0: {
      DCHECK_EQ(receiver_mode, ConvertReceiverMode::kNullOrUndefined);
      CallArguments args(receiver_mode);
      BuildCallWithFeedback(target, args, feedback_source);
      break;
    }
    case 1: {
      CallArguments args(receiver_mode, {LoadRegisterRaw(1)});
      BuildCallWithFeedback(target, args, feedback_source);
      break;
    }
    case 2: {
      CallArguments args(receiver_mode,
                         {LoadRegisterRaw(1), LoadRegisterRaw(2)});
      BuildCallWithFeedback(target, args, feedback_source);
      break;
    }
    case 3: {
      CallArguments args(receiver_mode, {LoadRegisterRaw(1), LoadRegisterRaw(2),
                                         LoadRegisterRaw(3)});
      BuildCallWithFeedback(target, args, feedback_source);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void MaglevGraphBuilder::VisitCallAnyReceiver() {
  BuildCallFromRegisterList(ConvertReceiverMode::kAny);
}
void MaglevGraphBuilder::VisitCallProperty() {
  BuildCallFromRegisterList(ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty0() {
  BuildCallFromRegisters(0, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty1() {
  BuildCallFromRegisters(1, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallProperty2() {
  BuildCallFromRegisters(2, ConvertReceiverMode::kNotNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver() {
  BuildCallFromRegisterList(ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver0() {
  BuildCallFromRegisters(0, ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver1() {
  BuildCallFromRegisters(1, ConvertReceiverMode::kNullOrUndefined);
}
void MaglevGraphBuilder::VisitCallUndefinedReceiver2() {
  BuildCallFromRegisters(2, ConvertReceiverMode::kNullOrUndefined);
}

void MaglevGraphBuilder::VisitCallWithSpread() {
  ValueNode* function = LoadRegisterTagged(0);
  interpreter::RegisterList reglist = iterator_.GetRegisterListOperand(1);
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source(feedback(), slot);
  CallArguments args(ConvertReceiverMode::kAny, reglist,
                     current_interpreter_frame_, CallArguments::kWithSpread);
  BuildCallWithFeedback(function, args, feedback_source);
}

void MaglevGraphBuilder::VisitCallRuntime() {
  Runtime::FunctionId function_id = iterator_.GetRuntimeIdOperand(0);
  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  ValueNode* context = GetContext();
  size_t input_count = args.register_count() + CallRuntime::kFixedInputCount;
  CallRuntime* call_runtime = AddNewNode<CallRuntime>(
      input_count,
      [&](CallRuntime* call_runtime) {
        for (int i = 0; i < args.register_count(); ++i) {
          call_runtime->set_arg(i, GetTaggedValue(args[i]));
        }
      },
      function_id, context);
  SetAccumulator(call_runtime);
}

void MaglevGraphBuilder::VisitCallJSRuntime() {
  // Get the function to call from the native context.
  compiler::NativeContextRef native_context = broker()->target_native_context();
  ValueNode* context = GetConstant(native_context);
  uint32_t slot = iterator_.GetNativeContextIndexOperand(0);
  ValueNode* callee = LoadAndCacheContextSlot(
      context, NativeContext::OffsetOfElementAt(slot), kMutable);
  // Call the function.
  interpreter::RegisterList reglist = iterator_.GetRegisterListOperand(1);
  CallArguments args(ConvertReceiverMode::kNullOrUndefined, reglist,
                     current_interpreter_frame_);
  SetAccumulator(BuildGenericCall(callee, Call::TargetType::kJSFunction, args));
}

void MaglevGraphBuilder::VisitCallRuntimeForPair() {
  Runtime::FunctionId function_id = iterator_.GetRuntimeIdOperand(0);
  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  ValueNode* context = GetContext();

  size_t input_count = args.register_count() + CallRuntime::kFixedInputCount;
  CallRuntime* call_runtime = AddNewNode<CallRuntime>(
      input_count,
      [&](CallRuntime* call_runtime) {
        for (int i = 0; i < args.register_count(); ++i) {
          call_runtime->set_arg(i, GetTaggedValue(args[i]));
        }
      },
      function_id, context);
  auto result = iterator_.GetRegisterPairOperand(3);
  StoreRegisterPair(result, call_runtime);
}

void MaglevGraphBuilder::VisitInvokeIntrinsic() {
  // InvokeIntrinsic <function_id> <first_arg> <arg_count>
  Runtime::FunctionId intrinsic_id = iterator_.GetIntrinsicIdOperand(0);
  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  switch (intrinsic_id) {
#define CASE(Name, _, arg_count)                                         \
  case Runtime::kInline##Name:                                           \
    DCHECK_IMPLIES(arg_count != -1, arg_count == args.register_count()); \
    VisitIntrinsic##Name(args);                                          \
    break;
    INTRINSICS_LIST(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}

void MaglevGraphBuilder::VisitIntrinsicCopyDataProperties(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kCopyDataProperties>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::
    VisitIntrinsicCopyDataPropertiesWithExcludedPropertiesOnStack(
        interpreter::RegisterList args) {
  SmiConstant* excluded_property_count =
      GetSmiConstant(args.register_count() - 1);
  int kContext = 1;
  int kExcludedPropertyCount = 1;
  CallBuiltin* call_builtin = AddNewNode<CallBuiltin>(
      args.register_count() + kContext + kExcludedPropertyCount,
      [&](CallBuiltin* call_builtin) {
        int arg_index = 0;
        call_builtin->set_arg(arg_index++, GetTaggedValue(args[0]));
        call_builtin->set_arg(arg_index++, excluded_property_count);
        for (int i = 1; i < args.register_count(); i++) {
          call_builtin->set_arg(arg_index++, GetTaggedValue(args[i]));
        }
      },
      Builtin::kCopyDataPropertiesWithExcludedProperties, GetContext());
  SetAccumulator(call_builtin);
}

void MaglevGraphBuilder::VisitIntrinsicCreateIterResultObject(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kCreateIterResultObject>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicCreateAsyncFromSyncIterator(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 1);
  SetAccumulator(
      BuildCallBuiltin<Builtin::kCreateAsyncFromSyncIteratorBaseline>(
          {GetTaggedValue(args[0])}));
}

void MaglevGraphBuilder::VisitIntrinsicCreateJSGeneratorObject(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kCreateGeneratorObject>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicGeneratorGetResumeMode(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 1);
  ValueNode* generator = GetTaggedValue(args[0]);
  SetAccumulator(AddNewNode<LoadTaggedField>(
      {generator}, JSGeneratorObject::kResumeModeOffset));
}

void MaglevGraphBuilder::VisitIntrinsicGeneratorClose(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 1);
  ValueNode* generator = GetTaggedValue(args[0]);
  ValueNode* value = GetSmiConstant(JSGeneratorObject::kGeneratorClosed);
  BuildStoreTaggedFieldNoWriteBarrier(generator, value,
                                      JSGeneratorObject::kContinuationOffset);
  SetAccumulator(GetRootConstant(RootIndex::kUndefinedValue));
}

void MaglevGraphBuilder::VisitIntrinsicGetImportMetaObject(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 0);
  SetAccumulator(BuildCallRuntime(Runtime::kGetImportMetaObject, {}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncFunctionAwaitCaught(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncFunctionAwaitCaught>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncFunctionAwaitUncaught(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncFunctionAwaitUncaught>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncFunctionEnter(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncFunctionEnter>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncFunctionReject(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncFunctionReject>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncFunctionResolve(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncFunctionResolve>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncGeneratorAwaitCaught(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncGeneratorAwaitCaught>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncGeneratorAwaitUncaught(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncGeneratorAwaitUncaught>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncGeneratorReject(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 2);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncGeneratorReject>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncGeneratorResolve(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 3);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncGeneratorResolve>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1]),
       GetTaggedValue(args[2])}));
}

void MaglevGraphBuilder::VisitIntrinsicAsyncGeneratorYieldWithAwait(
    interpreter::RegisterList args) {
  DCHECK_EQ(args.register_count(), 3);
  SetAccumulator(BuildCallBuiltin<Builtin::kAsyncGeneratorYieldWithAwait>(
      {GetTaggedValue(args[0]), GetTaggedValue(args[1]),
       GetTaggedValue(args[2])}));
}

ValueNode* MaglevGraphBuilder::BuildGenericConstruct(
    ValueNode* target, ValueNode* new_target, ValueNode* context,
    const CallArguments& args,
    const compiler::FeedbackSource& feedback_source) {
  size_t input_count = args.count_with_receiver() + Construct::kFixedInputCount;
  DCHECK_EQ(args.receiver_mode(), ConvertReceiverMode::kNullOrUndefined);
  return AddNewNode<Construct>(
      input_count,
      [&](Construct* construct) {
        int arg_index = 0;
        // Add undefined receiver.
        construct->set_arg(arg_index++,
                           GetRootConstant(RootIndex::kUndefinedValue));
        for (size_t i = 0; i < args.count(); i++) {
          construct->set_arg(arg_index++, GetTaggedValue(args[i]));
        }
      },
      feedback_source, target, new_target, context);
}

ReduceResult MaglevGraphBuilder::ReduceConstruct(
    compiler::HeapObjectRef feedback_target, ValueNode* target,
    ValueNode* new_target, CallArguments& args,
    compiler::FeedbackSource& feedback_source) {
  if (feedback_target.IsAllocationSite()) {
    // TODO(victorgomes): Inline array constructors.
    return ReduceResult::Fail();
  } else if (feedback_target.map(broker()).is_constructor()) {
    if (target != new_target) return ReduceResult::Fail();
    if (feedback_target.IsJSFunction()) {
      compiler::JSFunctionRef function = feedback_target.AsJSFunction();

      // Do not inline constructors with break points.
      compiler::SharedFunctionInfoRef sfi = function.shared(broker());
      if (sfi.HasBreakInfo(broker())) {
        return ReduceResult::Fail();
      }

      // Do not inline cross natives context.
      if (function.native_context(broker()) !=
          broker()->target_native_context()) {
        return ReduceResult::Fail();
      }

      if (args.mode() != CallArguments::kDefault) {
        // TODO(victorgomes): Maybe inline the spread stub? Or call known
        // function directly if arguments list is an array.
        return ReduceResult::Fail();
      }

      // TODO(victorgomes): specialize for known constants targets, specially
      // ArrayConstructor.

      if (sfi.construct_as_builtin()) {
        // TODO(victorgomes): Inline JSBuiltinsConstructStub.
        return ReduceResult::Fail();
      }

      RETURN_IF_ABORT(BuildCheckValue(target, function));

      int construct_arg_count = static_cast<int>(args.count());
      base::Vector<ValueNode*> construct_arguments_without_receiver =
          zone()->AllocateVector<ValueNode*>(construct_arg_count);
      for (int i = 0; i < construct_arg_count; i++) {
        construct_arguments_without_receiver[i] = args[i];
      }

      if (IsDerivedConstructor(sfi.kind())) {
        ValueNode* implicit_receiver =
            GetRootConstant(RootIndex::kTheHoleValue);
        args.set_receiver(implicit_receiver);
        ValueNode* call_result;
        {
          DeoptFrameScope construct(this, implicit_receiver);
          ReduceResult result = TryBuildCallKnownJSFunction(
              function, new_target, args, feedback_source);
          RETURN_IF_ABORT(result);
          call_result = result.value();
        }
        if (CheckType(call_result, NodeType::kJSReceiver)) return call_result;
        ValueNode* constant_node;
        if (compiler::OptionalHeapObjectRef maybe_constant =
                TryGetConstant(call_result, &constant_node)) {
          compiler::HeapObjectRef constant = maybe_constant.value();
          if (constant.IsJSReceiver()) return constant_node;
        }
        if (!call_result->properties().is_tagged()) {
          return BuildCallRuntime(Runtime::kThrowConstructorReturnedNonObject,
                                  {});
        }
        return AddNewNode<CheckDerivedConstructResult>({call_result});
      }

      // We do not create a construct stub lazy deopt frame, since
      // FastNewObject cannot fail if target is a JSFunction.
      ValueNode* implicit_receiver = nullptr;
      if (function.has_initial_map(broker())) {
        compiler::MapRef map = function.initial_map(broker());
        if (map.GetConstructor(broker()).equals(feedback_target)) {
          implicit_receiver = BuildAllocateFastObject(
              FastObject(function, zone(), broker()), AllocationType::kYoung);
          // TODO(leszeks): Don't eagerly clear the raw allocation, have the
          // next side effect clear it.
          ClearCurrentRawAllocation();
        }
      }
      if (implicit_receiver == nullptr) {
        implicit_receiver =
            BuildCallBuiltin<Builtin::kFastNewObject>({target, new_target});
      }
      EnsureType(implicit_receiver, NodeType::kJSReceiver);

      args.set_receiver(implicit_receiver);
      ValueNode* call_result;
      {
        DeoptFrameScope construct(this, implicit_receiver);
        ReduceResult result = TryBuildCallKnownJSFunction(
            function, new_target, args, feedback_source);
        RETURN_IF_ABORT(result);
        call_result = result.value();
      }
      if (CheckType(call_result, NodeType::kJSReceiver)) return call_result;
      if (!call_result->properties().is_tagged()) return implicit_receiver;
      ValueNode* constant_node;
      if (compiler::OptionalHeapObjectRef maybe_constant =
              TryGetConstant(call_result, &constant_node)) {
        compiler::HeapObjectRef constant = maybe_constant.value();
        DCHECK(CheckType(implicit_receiver, NodeType::kJSReceiver));
        if (constant.IsJSReceiver()) return constant_node;
        return implicit_receiver;
      }
      return AddNewNode<CheckConstructResult>({call_result, implicit_receiver});
    }
    // TODO(v8:7700): Add fast paths for other callables.
    return ReduceResult::Fail();
  } else {
    // TODO(victorgomes): Deal the case where target is not a constructor.
    return ReduceResult::Fail();
  }
}

void MaglevGraphBuilder::BuildConstruct(
    ValueNode* target, ValueNode* new_target, CallArguments& args,
    compiler::FeedbackSource& feedback_source) {
  compiler::ProcessedFeedback const& processed_feedback =
      broker()->GetFeedbackForCall(feedback_source);
  if (processed_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForConstruct));
  }

  DCHECK_EQ(processed_feedback.kind(), compiler::ProcessedFeedback::kCall);
  compiler::OptionalHeapObjectRef feedback_target =
      processed_feedback.AsCall().target();
  if (feedback_target.has_value()) {
    PROCESS_AND_RETURN_IF_DONE(
        ReduceConstruct(feedback_target.value(), target, new_target, args,
                        feedback_source),
        SetAccumulator);
  }

  if (compiler::OptionalHeapObjectRef maybe_constant = TryGetConstant(target)) {
    PROCESS_AND_RETURN_IF_DONE(
        ReduceConstruct(maybe_constant.value(), target, new_target, args,
                        feedback_source),
        SetAccumulator);
  }

  ValueNode* context = GetContext();
  SetAccumulator(BuildGenericConstruct(target, new_target, context, args,
                                       feedback_source));
}

void MaglevGraphBuilder::VisitConstruct() {
  ValueNode* new_target = GetAccumulatorTagged();
  ValueNode* target = LoadRegisterTagged(0);
  interpreter::RegisterList reg_list = iterator_.GetRegisterListOperand(1);
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  CallArguments args(ConvertReceiverMode::kNullOrUndefined, reg_list,
                     current_interpreter_frame_);
  BuildConstruct(target, new_target, args, feedback_source);
}

void MaglevGraphBuilder::VisitConstructWithSpread() {
  ValueNode* new_target = GetAccumulatorTagged();
  ValueNode* constructor = LoadRegisterTagged(0);
  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  ValueNode* context = GetContext();
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source(feedback(), slot);

  int kReceiver = 1;
  size_t input_count =
      args.register_count() + kReceiver + ConstructWithSpread::kFixedInputCount;
  ConstructWithSpread* construct = AddNewNode<ConstructWithSpread>(
      input_count,
      [&](ConstructWithSpread* construct) {
        int arg_index = 0;
        // Add undefined receiver.
        construct->set_arg(arg_index++,
                           GetRootConstant(RootIndex::kUndefinedValue));
        for (int i = 0; i < args.register_count(); i++) {
          construct->set_arg(arg_index++, GetTaggedValue(args[i]));
        }
      },
      feedback_source, constructor, new_target, context);
  SetAccumulator(construct);
}

void MaglevGraphBuilder::VisitTestEqual() {
  VisitCompareOperation<Operation::kEqual>();
}
void MaglevGraphBuilder::VisitTestEqualStrict() {
  VisitCompareOperation<Operation::kStrictEqual>();
}
void MaglevGraphBuilder::VisitTestLessThan() {
  VisitCompareOperation<Operation::kLessThan>();
}
void MaglevGraphBuilder::VisitTestLessThanOrEqual() {
  VisitCompareOperation<Operation::kLessThanOrEqual>();
}
void MaglevGraphBuilder::VisitTestGreaterThan() {
  VisitCompareOperation<Operation::kGreaterThan>();
}
void MaglevGraphBuilder::VisitTestGreaterThanOrEqual() {
  VisitCompareOperation<Operation::kGreaterThanOrEqual>();
}

MaglevGraphBuilder::InferHasInPrototypeChainResult
MaglevGraphBuilder::InferHasInPrototypeChain(
    ValueNode* receiver, compiler::HeapObjectRef prototype) {
  auto node_info = known_node_aspects().TryGetInfoFor(receiver);
  // If the map set is not found, then we don't know anything about the map of
  // the receiver, so bail.
  if (!node_info || !node_info->possible_maps_are_known()) {
    return kMayBeInPrototypeChain;
  }

  // If the set of possible maps is empty, then there's no possible map for this
  // receiver, therefore this path is unreachable at runtime. We're unlikely to
  // ever hit this case, BuildCheckMaps should already unconditionally deopt,
  // but check it in case another checking operation fails to statically
  // unconditionally deopt.
  if (node_info->possible_maps().is_empty()) {
    // TODO(leszeks): Add an unreachable assert here.
    return kIsNotInPrototypeChain;
  }

  ZoneVector<compiler::MapRef> receiver_map_refs(zone());

  // Try to determine either that all of the {receiver_maps} have the given
  // {prototype} in their chain, or that none do. If we can't tell, return
  // kMayBeInPrototypeChain.
  bool all = true;
  bool none = true;
  for (compiler::MapRef map : node_info->possible_maps()) {
    receiver_map_refs.push_back(map);
    while (true) {
      if (IsSpecialReceiverInstanceType(map.instance_type())) {
        return kMayBeInPrototypeChain;
      }
      if (!map.IsJSObjectMap()) {
        all = false;
        break;
      }
      compiler::HeapObjectRef map_prototype = map.prototype(broker());
      if (map_prototype.equals(prototype)) {
        none = false;
        break;
      }
      map = map_prototype.map(broker());
      // TODO(v8:11457) Support dictionary mode protoypes here.
      if (!map.is_stable() || map.is_dictionary_map()) {
        return kMayBeInPrototypeChain;
      }
      if (map.oddball_type(broker()) == compiler::OddballType::kNull) {
        all = false;
        break;
      }
    }
  }
  DCHECK(!receiver_map_refs.empty());
  DCHECK_IMPLIES(all, !none);
  if (!all && !none) return kMayBeInPrototypeChain;

  {
    compiler::OptionalJSObjectRef last_prototype;
    if (all) {
      // We don't need to protect the full chain if we found the prototype, we
      // can stop at {prototype}.  In fact we could stop at the one before
      // {prototype} but since we're dealing with multiple receiver maps this
      // might be a different object each time, so it's much simpler to include
      // {prototype}. That does, however, mean that we must check {prototype}'s
      // map stability.
      if (!prototype.map(broker()).is_stable()) return kMayBeInPrototypeChain;
      last_prototype = prototype.AsJSObject();
    }
    broker()->dependencies()->DependOnStablePrototypeChains(
        receiver_map_refs, kStartAtPrototype, last_prototype);
  }

  DCHECK_EQ(all, !none);
  return all ? kIsInPrototypeChain : kIsNotInPrototypeChain;
}

ReduceResult MaglevGraphBuilder::TryBuildFastHasInPrototypeChain(
    ValueNode* object, compiler::HeapObjectRef prototype) {
  auto in_prototype_chain = InferHasInPrototypeChain(object, prototype);
  if (in_prototype_chain == kMayBeInPrototypeChain) return ReduceResult::Fail();

  return GetBooleanConstant(in_prototype_chain == kIsInPrototypeChain);
}

ReduceResult MaglevGraphBuilder::BuildHasInPrototypeChain(
    ValueNode* object, compiler::HeapObjectRef prototype) {
  RETURN_IF_DONE(TryBuildFastHasInPrototypeChain(object, prototype));
  return AddNewNode<HasInPrototypeChain>({object}, prototype);
}

ReduceResult MaglevGraphBuilder::TryBuildFastOrdinaryHasInstance(
    ValueNode* object, compiler::JSObjectRef callable,
    ValueNode* callable_node_if_not_constant) {
  const bool is_constant = callable_node_if_not_constant == nullptr;
  if (!is_constant) return ReduceResult::Fail();

  if (callable.IsJSBoundFunction()) {
    // OrdinaryHasInstance on bound functions turns into a recursive
    // invocation of the instanceof operator again.
    compiler::JSBoundFunctionRef function = callable.AsJSBoundFunction();
    compiler::JSReceiverRef bound_target_function =
        function.bound_target_function(broker());

    if (bound_target_function.IsJSObject()) {
      RETURN_IF_DONE(TryBuildFastInstanceOf(
          object, bound_target_function.AsJSObject(), nullptr));
    }

    // If we can't build a fast instance-of, build a slow one with the
    // partial optimisation of using the bound target function constant.
    return BuildCallBuiltin<Builtin::kInstanceOf>(
        {object, GetConstant(bound_target_function)});
  }

  if (callable.IsJSFunction()) {
    // Optimize if we currently know the "prototype" property.
    compiler::JSFunctionRef function = callable.AsJSFunction();

    // TODO(v8:7700): Remove the has_prototype_slot condition once the broker
    // is always enabled.
    if (!function.map(broker()).has_prototype_slot() ||
        !function.has_instance_prototype(broker()) ||
        function.PrototypeRequiresRuntimeLookup(broker())) {
      return ReduceResult::Fail();
    }

    compiler::HeapObjectRef prototype =
        broker()->dependencies()->DependOnPrototypeProperty(function);
    return BuildHasInPrototypeChain(object, prototype);
  }

  return ReduceResult::Fail();
}

ReduceResult MaglevGraphBuilder::BuildOrdinaryHasInstance(
    ValueNode* object, compiler::JSObjectRef callable,
    ValueNode* callable_node_if_not_constant) {
  RETURN_IF_DONE(TryBuildFastOrdinaryHasInstance(
      object, callable, callable_node_if_not_constant));

  return BuildCallBuiltin<Builtin::kOrdinaryHasInstance>(
      {callable_node_if_not_constant ? callable_node_if_not_constant
                                     : GetConstant(callable),
       object});
}

ReduceResult MaglevGraphBuilder::TryBuildFastInstanceOf(
    ValueNode* object, compiler::JSObjectRef callable,
    ValueNode* callable_node_if_not_constant) {
  compiler::MapRef receiver_map = callable.map(broker());
  compiler::NameRef name = broker()->has_instance_symbol();
  compiler::PropertyAccessInfo access_info = broker()->GetPropertyAccessInfo(
      receiver_map, name, compiler::AccessMode::kLoad);

  // TODO(v8:11457) Support dictionary mode holders here.
  if (access_info.IsInvalid() || access_info.HasDictionaryHolder()) {
    return ReduceResult::Fail();
  }
  access_info.RecordDependencies(broker()->dependencies());

  if (access_info.IsNotFound()) {
    // If there's no @@hasInstance handler, the OrdinaryHasInstance operation
    // takes over, but that requires the constructor to be callable.
    if (!receiver_map.is_callable()) {
      return ReduceResult::Fail();
    }

    broker()->dependencies()->DependOnStablePrototypeChains(
        access_info.lookup_start_object_maps(), kStartAtPrototype);

    // Monomorphic property access.
    if (callable_node_if_not_constant) {
      RETURN_IF_ABORT(BuildCheckMaps(
          callable_node_if_not_constant,
          base::VectorOf(access_info.lookup_start_object_maps())));
    } else {
      // Even if we have a constant receiver, we still have to make sure its
      // map is correct, in case it migrates.
      if (receiver_map.is_stable()) {
        broker()->dependencies()->DependOnStableMap(receiver_map);
      } else {
        RETURN_IF_ABORT(BuildCheckMaps(
            GetConstant(callable),
            base::VectorOf(access_info.lookup_start_object_maps())));
      }
    }

    return BuildOrdinaryHasInstance(object, callable,
                                    callable_node_if_not_constant);
  }

  if (access_info.IsFastDataConstant()) {
    compiler::OptionalJSObjectRef holder = access_info.holder();
    bool found_on_proto = holder.has_value();
    compiler::JSObjectRef holder_ref =
        found_on_proto ? holder.value() : callable;
    compiler::OptionalObjectRef has_instance_field =
        holder_ref.GetOwnFastDataProperty(
            broker(), access_info.field_representation(),
            access_info.field_index(), broker()->dependencies());
    if (!has_instance_field.has_value() ||
        !has_instance_field->IsHeapObject() ||
        !has_instance_field->AsHeapObject().map(broker()).is_callable()) {
      return ReduceResult::Fail();
    }

    if (found_on_proto) {
      broker()->dependencies()->DependOnStablePrototypeChains(
          access_info.lookup_start_object_maps(), kStartAtPrototype,
          holder.value());
    }

    ValueNode* callable_node;
    if (callable_node_if_not_constant) {
      // Check that {callable_node_if_not_constant} is actually {callable}.
      RETURN_IF_ABORT(BuildCheckValue(callable_node_if_not_constant, callable));
      callable_node = callable_node_if_not_constant;
    } else {
      callable_node = GetConstant(callable);
    }
    RETURN_IF_ABORT(BuildCheckMaps(
        callable_node, base::VectorOf(access_info.lookup_start_object_maps())));

    // Special case the common case, where @@hasInstance is
    // Function.p.hasInstance. In this case we don't need to call ToBoolean (or
    // use the continuation), since OrdinaryHasInstance is guaranteed to return
    // a boolean.
    if (has_instance_field->IsJSFunction()) {
      compiler::SharedFunctionInfoRef shared =
          has_instance_field->AsJSFunction().shared(broker());
      if (shared.HasBuiltinId() &&
          shared.builtin_id() == Builtin::kFunctionPrototypeHasInstance) {
        return BuildOrdinaryHasInstance(object, callable,
                                        callable_node_if_not_constant);
      }
    }

    // Call @@hasInstance
    CallArguments args(ConvertReceiverMode::kNotNullOrUndefined,
                       {callable_node, object});
    ValueNode* call_result;
    {
      // Make sure that a lazy deopt after the @@hasInstance call also performs
      // ToBoolean before returning to the interpreter.
      DeoptFrameScope continuation_scope(
          this, Builtin::kToBooleanLazyDeoptContinuation);

      if (has_instance_field->IsJSFunction()) {
        ReduceResult result =
            ReduceCallForConstant(has_instance_field->AsJSFunction(), args);
        DCHECK(!result.IsDoneWithAbort());
        call_result = result.value();
      } else {
        call_result = BuildGenericCall(GetConstant(*has_instance_field),
                                       Call::TargetType::kAny, args);
      }
      // TODO(victorgomes): Propagate the case if we need to soft deopt.
    }

    BuildToBoolean(GetTaggedValue(call_result));
    return ReduceResult::Done();
  }

  return ReduceResult::Fail();
}

template <bool flip>
void MaglevGraphBuilder::BuildToBoolean(ValueNode* value) {
  if (IsConstantNode(value->opcode())) {
    SetAccumulator(
        GetBooleanConstant(FromConstantToBool(local_isolate(), value) ^ flip));
    return;
  }
  NodeType value_type;
  if (CheckType(value, NodeType::kJSReceiver, &value_type)) {
    SetAccumulator(GetBooleanConstant(!flip));
    return;
  }
  ValueNode* falsy_value = nullptr;
  if (CheckType(value, NodeType::kString)) {
    falsy_value = GetRootConstant(RootIndex::kempty_string);
  } else if (CheckType(value, NodeType::kSmi)) {
    falsy_value = GetSmiConstant(0);
  }
  if (falsy_value != nullptr) {
    // Negate flip because we're comparing with a falsy value.
    if (!TryBuildBranchFor<BranchIfReferenceCompare, !flip>(
            {value, falsy_value}, Operation::kStrictEqual)) {
      SetAccumulator(
          AddNewNode<std::conditional_t<flip, TaggedEqual, TaggedNotEqual>>(
              {value, falsy_value}));
    }
    return;
  }
  if (CheckType(value, NodeType::kBoolean)) {
    if (!TryBuildBranchFor<BranchIfRootConstant>(
            {value}, flip ? RootIndex::kFalseValue : RootIndex::kTrueValue)) {
      if (flip) {
        value = AddNewNode<LogicalNot>({value});
      }
      SetAccumulator(value);
    }
    return;
  }
  if (!TryBuildBranchFor<BranchIfToBooleanTrue, flip>(
          {value}, GetCheckType(value_type))) {
    SetAccumulator(
        AddNewNode<std::conditional_t<flip, ToBooleanLogicalNot, ToBoolean>>(
            {value}, GetCheckType(value_type)));
  }
}

ReduceResult MaglevGraphBuilder::TryBuildFastInstanceOfWithFeedback(
    ValueNode* object, ValueNode* callable,
    compiler::FeedbackSource feedback_source) {
  compiler::ProcessedFeedback const& feedback =
      broker()->GetFeedbackForInstanceOf(feedback_source);

  if (feedback.IsInsufficient()) {
    return EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForInstanceOf);
  }

  // Check if the right hand side is a known receiver, or
  // we have feedback from the InstanceOfIC.
  compiler::OptionalHeapObjectRef maybe_constant;
  if ((maybe_constant = TryGetConstant(callable)) &&
      maybe_constant.value().IsJSObject()) {
    compiler::JSObjectRef callable_ref = maybe_constant.value().AsJSObject();
    return TryBuildFastInstanceOf(object, callable_ref, nullptr);
  }
  if (feedback_source.IsValid()) {
    compiler::OptionalJSObjectRef callable_from_feedback =
        feedback.AsInstanceOf().value();
    if (callable_from_feedback) {
      return TryBuildFastInstanceOf(object, *callable_from_feedback, callable);
    }
  }
  return ReduceResult::Fail();
}

void MaglevGraphBuilder::VisitTestInstanceOf() {
  // TestInstanceOf <src> <feedback_slot>
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* callable = GetAccumulatorTagged();
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  ReduceResult result =
      TryBuildFastInstanceOfWithFeedback(object, callable, feedback_source);
  PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

  ValueNode* context = GetContext();
  SetAccumulator(
      AddNewNode<TestInstanceOf>({context, object, callable}, feedback_source));
}

void MaglevGraphBuilder::VisitTestIn() {
  // TestIn <src> <feedback_slot>
  ValueNode* object = GetAccumulatorTagged();
  ValueNode* name = LoadRegisterTagged(0);
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  // TODO(victorgomes): Create fast path using feedback.
  USE(feedback_source);

  SetAccumulator(
      BuildCallBuiltin<Builtin::kKeyedHasIC>({object, name}, feedback_source));
}

void MaglevGraphBuilder::VisitToName() {
  // ToObject <dst>
  ValueNode* value = GetAccumulatorTagged();
  if (CheckType(value, NodeType::kName)) {
    SetAccumulator(value);
  } else {
    SetAccumulator(AddNewNode<ToName>({GetContext(), value}));
  }
}

ValueNode* MaglevGraphBuilder::BuildToString(ValueNode* value,
                                             ToString::ConversionMode mode) {
  if (CheckType(value, NodeType::kString)) return value;
  // TODO(victorgomes): Add fast path for constant primitives.
  if (CheckType(value, NodeType::kNumber)) {
    // TODO(verwaest): Float64ToString if float.
    return AddNewNode<NumberToString>({GetTaggedValue(value)});
  }
  return AddNewNode<ToString>({GetContext(), GetTaggedValue(value)}, mode);
}

void MaglevGraphBuilder::BuildToNumberOrToNumeric(Object::Conversion mode) {
  ValueNode* value = GetRawAccumulator();
  switch (value->value_representation()) {
    case ValueRepresentation::kInt32:
    case ValueRepresentation::kUint32:
    case ValueRepresentation::kFloat64:
      return;

    case ValueRepresentation::kHoleyFloat64: {
      SetAccumulator(AddNewNode<HoleyFloat64ToMaybeNanFloat64>({value}));
      return;
    }

    case ValueRepresentation::kTagged:
      // We'll insert the required checks depending on the feedback.
      break;

    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }

  FeedbackSlot slot = GetSlotOperand(0);
  switch (broker()->GetFeedbackForBinaryOperation(
      compiler::FeedbackSource(feedback(), slot))) {
    case BinaryOperationHint::kSignedSmall:
      BuildCheckSmi(value);
      break;
    case BinaryOperationHint::kSignedSmallInputs:
      UNREACHABLE();
    case BinaryOperationHint::kNumber:
    case BinaryOperationHint::kBigInt:
    case BinaryOperationHint::kBigInt64:
      if (mode == Object::Conversion::kToNumber &&
          EnsureType(value, NodeType::kNumber)) {
        return;
      }
      AddNewNode<CheckNumber>({value}, mode);
      break;
    case BinaryOperationHint::kNone:
    // TODO(leszeks): Faster ToNumber for kNumberOrOddball
    case BinaryOperationHint::kNumberOrOddball:
    case BinaryOperationHint::kString:
    case BinaryOperationHint::kAny:
      if (CheckType(value, NodeType::kNumber)) return;
      SetAccumulator(AddNewNode<ToNumberOrNumeric>({value}, mode));
      break;
  }
}

void MaglevGraphBuilder::VisitToNumber() {
  BuildToNumberOrToNumeric(Object::Conversion::kToNumber);
}
void MaglevGraphBuilder::VisitToNumeric() {
  BuildToNumberOrToNumeric(Object::Conversion::kToNumeric);
}

void MaglevGraphBuilder::VisitToObject() {
  // ToObject <dst>
  ValueNode* value = GetAccumulatorTagged();
  interpreter::Register destination = iterator_.GetRegisterOperand(0);
  NodeType old_type;
  if (CheckType(value, NodeType::kJSReceiver, &old_type)) {
    MoveNodeBetweenRegisters(interpreter::Register::virtual_accumulator(),
                             destination);
  } else {
    StoreRegister(destination, AddNewNode<ToObject>({GetContext(), value},
                                                    GetCheckType(old_type)));
  }
}

void MaglevGraphBuilder::VisitToString() {
  // ToString
  ValueNode* value = GetAccumulatorTagged();
  SetAccumulator(BuildToString(value, ToString::kThrowOnSymbol));
}

void MaglevGraphBuilder::VisitToBoolean() {
  BuildToBoolean(GetAccumulatorTagged());
}

void FastObject::ClearFields() {
  for (int i = 0; i < inobject_properties; i++) {
    fields[i] = FastField();
  }
}

FastObject::FastObject(compiler::JSFunctionRef constructor, Zone* zone,
                       compiler::JSHeapBroker* broker)
    : map(constructor.initial_map(broker)) {
  compiler::SlackTrackingPrediction prediction =
      broker->dependencies()->DependOnInitialMapInstanceSizePrediction(
          constructor);
  inobject_properties = prediction.inobject_property_count();
  instance_size = prediction.instance_size();
  fields = zone->AllocateArray<FastField>(inobject_properties);
  ClearFields();
  elements = FastFixedArray();
}

void MaglevGraphBuilder::VisitCreateRegExpLiteral() {
  // CreateRegExpLiteral <pattern_idx> <literal_idx> <flags>
  compiler::StringRef pattern = GetRefOperand<String>(0);
  FeedbackSlot slot = GetSlotOperand(1);
  uint32_t flags = GetFlag16Operand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  // TODO(victorgomes): Inline allocation if feedback has a RegExpLiteral.
  SetAccumulator(
      AddNewNode<CreateRegExpLiteral>({}, pattern, feedback_source, flags));
}

void MaglevGraphBuilder::VisitCreateArrayLiteral() {
  compiler::HeapObjectRef constant_elements = GetRefOperand<HeapObject>(0);
  FeedbackSlot slot_index = GetSlotOperand(1);
  int bytecode_flags = GetFlag8Operand(2);
  int literal_flags =
      interpreter::CreateArrayLiteralFlags::FlagsBits::decode(bytecode_flags);
  compiler::FeedbackSource feedback_source(feedback(), slot_index);

  compiler::ProcessedFeedback const& processed_feedback =
      broker()->GetFeedbackForArrayOrObjectLiteral(feedback_source);

  if (processed_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForArrayLiteral));
  }

  ReduceResult result =
      TryBuildFastCreateObjectOrArrayLiteral(processed_feedback.AsLiteral());
  PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

  if (interpreter::CreateArrayLiteralFlags::FastCloneSupportedBit::decode(
          bytecode_flags)) {
    // TODO(victorgomes): CreateShallowArrayLiteral should not need the
    // boilerplate descriptor. However the current builtin checks that the
    // feedback exists and fallsback to CreateArrayLiteral if it doesn't.
    SetAccumulator(AddNewNode<CreateShallowArrayLiteral>(
        {}, constant_elements, feedback_source, literal_flags));
  } else {
    SetAccumulator(AddNewNode<CreateArrayLiteral>(
        {}, constant_elements, feedback_source, literal_flags));
  }
}

void MaglevGraphBuilder::VisitCreateArrayFromIterable() {
  ValueNode* iterable = GetAccumulatorTagged();
  SetAccumulator(
      BuildCallBuiltin<Builtin::kIterableToListWithSymbolLookup>({iterable}));
}

void MaglevGraphBuilder::VisitCreateEmptyArrayLiteral() {
  FeedbackSlot slot_index = GetSlotOperand(0);
  compiler::FeedbackSource feedback_source(feedback(), slot_index);
  compiler::ProcessedFeedback const& processed_feedback =
      broker()->GetFeedbackForArrayOrObjectLiteral(feedback_source);
  if (processed_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForArrayLiteral));
  }
  compiler::AllocationSiteRef site = processed_feedback.AsLiteral().value();

  broker()->dependencies()->DependOnElementsKind(site);
  ElementsKind kind = site.GetElementsKind();

  compiler::NativeContextRef native_context = broker()->target_native_context();
  compiler::MapRef map = native_context.GetInitialJSArrayMap(broker(), kind);
  FastObject literal(map, zone(), {});
  literal.js_array_length = MakeRef(broker(), Object::cast(Smi::zero()));
  SetAccumulator(BuildAllocateFastObject(literal, AllocationType::kYoung));
  // TODO(leszeks): Don't eagerly clear the raw allocation, have the next side
  // effect clear it.
  ClearCurrentRawAllocation();
}

base::Optional<FastObject> MaglevGraphBuilder::TryReadBoilerplateForFastLiteral(
    compiler::JSObjectRef boilerplate, AllocationType allocation, int max_depth,
    int* max_properties) {
  DCHECK_GE(max_depth, 0);
  DCHECK_GE(*max_properties, 0);

  if (max_depth == 0) return {};

  // Prevent concurrent migrations of boilerplate objects.
  compiler::JSHeapBroker::BoilerplateMigrationGuardIfNeeded
      boilerplate_access_guard(broker());

  // Now that we hold the migration lock, get the current map.
  compiler::MapRef boilerplate_map = boilerplate.map(broker());
  // Protect against concurrent changes to the boilerplate object by checking
  // for an identical value at the end of the compilation.
  broker()->dependencies()->DependOnObjectSlotValue(
      boilerplate, HeapObject::kMapOffset, boilerplate_map);
  {
    compiler::OptionalMapRef current_boilerplate_map =
        boilerplate.map_direct_read(broker());
    if (!current_boilerplate_map.has_value() ||
        !current_boilerplate_map->equals(boilerplate_map)) {
      // TODO(leszeks): Emit an eager deopt for this case, so that we can
      // re-learn the boilerplate. This will be easier once we get rid of the
      // two-pass approach, since we'll be able to create the eager deopt here
      // and return a ReduceResult::DoneWithAbort().
      return {};
    }
  }

  // Bail out if the boilerplate map has been deprecated.  The map could of
  // course be deprecated at some point after the line below, but it's not a
  // correctness issue -- it only means the literal won't be created with the
  // most up to date map(s).
  if (boilerplate_map.is_deprecated()) return {};

  // We currently only support in-object properties.
  if (boilerplate.map(broker()).elements_kind() == DICTIONARY_ELEMENTS ||
      boilerplate.map(broker()).is_dictionary_map() ||
      !boilerplate.raw_properties_or_hash(broker()).has_value()) {
    return {};
  }
  {
    compiler::ObjectRef properties =
        *boilerplate.raw_properties_or_hash(broker());
    bool const empty =
        properties.IsSmi() ||
        properties.equals(MakeRef(
            broker(), local_isolate()->factory()->empty_fixed_array())) ||
        properties.equals(MakeRef(
            broker(), Handle<Object>::cast(
                          local_isolate()->factory()->empty_property_array())));
    if (!empty) return {};
  }

  compiler::OptionalFixedArrayBaseRef maybe_elements =
      boilerplate.elements(broker(), kRelaxedLoad);
  if (!maybe_elements.has_value()) return {};
  compiler::FixedArrayBaseRef boilerplate_elements = maybe_elements.value();
  broker()->dependencies()->DependOnObjectSlotValue(
      boilerplate, JSObject::kElementsOffset, boilerplate_elements);
  int const elements_length = boilerplate_elements.length();

  FastObject fast_literal(boilerplate_map, zone(), {});

  // Compute the in-object properties to store first.
  int index = 0;
  for (InternalIndex i :
       InternalIndex::Range(boilerplate_map.NumberOfOwnDescriptors())) {
    PropertyDetails const property_details =
        boilerplate_map.GetPropertyDetails(broker(), i);
    if (property_details.location() != PropertyLocation::kField) continue;
    DCHECK_EQ(PropertyKind::kData, property_details.kind());
    if ((*max_properties)-- == 0) return {};

    int offset = boilerplate_map.GetInObjectPropertyOffset(index);
#ifdef DEBUG
    FieldIndex field_index =
        FieldIndex::ForDetails(*boilerplate_map.object(), property_details);
    DCHECK(field_index.is_inobject());
    DCHECK_EQ(index, field_index.property_index());
    DCHECK_EQ(field_index.offset(), offset);
#endif

    // Note: the use of RawInobjectPropertyAt (vs. the higher-level
    // GetOwnFastDataProperty) here is necessary, since the underlying value
    // may be `uninitialized`, which the latter explicitly does not support.
    compiler::OptionalObjectRef maybe_boilerplate_value =
        boilerplate.RawInobjectPropertyAt(
            broker(),
            FieldIndex::ForInObjectOffset(offset, FieldIndex::kTagged));
    if (!maybe_boilerplate_value.has_value()) return {};

    // Note: We don't need to take a compilation dependency verifying the value
    // of `boilerplate_value`, since boilerplate properties are constant after
    // initialization modulo map migration. We protect against concurrent map
    // migrations (other than elements kind transition, which don't affect us)
    // via the boilerplate_migration_access lock.
    compiler::ObjectRef boilerplate_value = maybe_boilerplate_value.value();

    FastField value;
    if (boilerplate_value.IsJSObject()) {
      compiler::JSObjectRef boilerplate_object = boilerplate_value.AsJSObject();
      base::Optional<FastObject> maybe_object_value =
          TryReadBoilerplateForFastLiteral(boilerplate_object, allocation,
                                           max_depth - 1, max_properties);
      if (!maybe_object_value.has_value()) return {};
      value = FastField(maybe_object_value.value());
    } else if (property_details.representation().IsDouble()) {
      Float64 number =
          Float64::FromBits(boilerplate_value.AsHeapNumber().value_as_bits());
      value = FastField(number);
    } else {
      // It's fine to store the 'uninitialized' Oddball into a Smi field since
      // it will get overwritten anyway.
      DCHECK_IMPLIES(property_details.representation().IsSmi() &&
                         !boilerplate_value.IsSmi(),
                     IsUninitialized(*boilerplate_value.object()));
      value = FastField(boilerplate_value);
    }

    DCHECK_LT(index, boilerplate_map.GetInObjectProperties());
    fast_literal.fields[index] = value;
    index++;
  }

  // Fill slack at the end of the boilerplate object with filler maps.
  int const boilerplate_length = boilerplate_map.GetInObjectProperties();
  for (; index < boilerplate_length; ++index) {
    DCHECK(!V8_MAP_PACKING_BOOL);
    // TODO(wenyuzhao): Fix incorrect MachineType when V8_MAP_PACKING is
    // enabled.
    DCHECK_LT(index, boilerplate_map.GetInObjectProperties());
    fast_literal.fields[index] = FastField(MakeRef(
        broker(), local_isolate()->factory()->one_pointer_filler_map()));
  }

  // Empty or copy-on-write elements just store a constant.
  compiler::MapRef elements_map = boilerplate_elements.map(broker());
  // Protect against concurrent changes to the boilerplate object by checking
  // for an identical value at the end of the compilation.
  broker()->dependencies()->DependOnObjectSlotValue(
      boilerplate_elements, HeapObject::kMapOffset, elements_map);
  if (boilerplate_elements.length() == 0 ||
      elements_map.IsFixedCowArrayMap(broker())) {
    if (allocation == AllocationType::kOld &&
        !boilerplate.IsElementsTenured(boilerplate_elements)) {
      return {};
    }
    fast_literal.elements = FastFixedArray(boilerplate_elements);
  } else {
    // Compute the elements to store first (might have effects).
    if (boilerplate_elements.IsFixedDoubleArray()) {
      int const size = FixedDoubleArray::SizeFor(elements_length);
      if (size > kMaxRegularHeapObjectSize) return {};
      fast_literal.elements = FastFixedArray(elements_length, zone(), double{});

      compiler::FixedDoubleArrayRef elements =
          boilerplate_elements.AsFixedDoubleArray();
      for (int i = 0; i < elements_length; ++i) {
        Float64 value = elements.GetFromImmutableFixedDoubleArray(i);
        fast_literal.elements.double_values[i] = value;
      }
    } else {
      int const size = FixedArray::SizeFor(elements_length);
      if (size > kMaxRegularHeapObjectSize) return {};
      fast_literal.elements = FastFixedArray(elements_length, zone());

      compiler::FixedArrayRef elements = boilerplate_elements.AsFixedArray();
      for (int i = 0; i < elements_length; ++i) {
        if ((*max_properties)-- == 0) return {};
        compiler::OptionalObjectRef element_value =
            elements.TryGet(broker(), i);
        if (!element_value.has_value()) return {};
        if (element_value->IsJSObject()) {
          base::Optional<FastObject> object = TryReadBoilerplateForFastLiteral(
              element_value->AsJSObject(), allocation, max_depth - 1,
              max_properties);
          if (!object.has_value()) return {};
          fast_literal.elements.values[i] = FastField(*object);
        } else {
          fast_literal.elements.values[i] = FastField(*element_value);
        }
      }
    }
  }

  if (boilerplate.IsJSArray()) {
    fast_literal.js_array_length =
        boilerplate.AsJSArray().GetBoilerplateLength(broker());
  }

  return fast_literal;
}

ValueNode* MaglevGraphBuilder::ExtendOrReallocateCurrentRawAllocation(
    int size, AllocationType allocation_type) {
  if (!current_raw_allocation_ ||
      current_raw_allocation_->allocation_type() != allocation_type ||
      !v8_flags.inline_new) {
    current_raw_allocation_ =
        AddNewNode<AllocateRaw>({}, allocation_type, size);
    return current_raw_allocation_;
  }

  int current_size = current_raw_allocation_->size();
  if (current_size + size > kMaxRegularHeapObjectSize) {
    return current_raw_allocation_ =
               AddNewNode<AllocateRaw>({}, allocation_type, size);
  }

  DCHECK_GT(current_size, 0);
  int previous_end = current_size;
  current_raw_allocation_->extend(size);
  return AddNewNode<FoldedAllocation>({current_raw_allocation_}, previous_end);
}

void MaglevGraphBuilder::ClearCurrentRawAllocation() {
  current_raw_allocation_ = nullptr;
}

ValueNode* MaglevGraphBuilder::BuildAllocateFastObject(
    FastObject object, AllocationType allocation_type) {
  SmallZoneVector<ValueNode*, 8> properties(object.inobject_properties, zone());
  for (int i = 0; i < object.inobject_properties; ++i) {
    properties[i] = BuildAllocateFastObject(object.fields[i], allocation_type);
  }
  ValueNode* elements =
      BuildAllocateFastObject(object.elements, allocation_type);

  DCHECK(object.map.IsJSObjectMap());
  // TODO(leszeks): Fold allocations.
  ValueNode* allocation = ExtendOrReallocateCurrentRawAllocation(
      object.instance_size, allocation_type);
  BuildStoreReceiverMap(allocation, object.map);
  AddNewNode<StoreTaggedFieldNoWriteBarrier>(
      {allocation, GetRootConstant(RootIndex::kEmptyFixedArray)},
      JSObject::kPropertiesOrHashOffset);
  if (object.js_array_length.has_value()) {
    BuildStoreTaggedField(allocation, GetConstant(*object.js_array_length),
                          JSArray::kLengthOffset);
  }

  BuildStoreTaggedField(allocation, elements, JSObject::kElementsOffset);
  for (int i = 0; i < object.inobject_properties; ++i) {
    BuildStoreTaggedField(allocation, properties[i],
                          object.map.GetInObjectPropertyOffset(i));
  }
  return allocation;
}

ValueNode* MaglevGraphBuilder::BuildAllocateFastObject(
    FastField value, AllocationType allocation_type) {
  switch (value.type) {
    case FastField::kObject:
      return BuildAllocateFastObject(value.object, allocation_type);
    case FastField::kMutableDouble: {
      ValueNode* new_alloc = ExtendOrReallocateCurrentRawAllocation(
          HeapNumber::kSize, allocation_type);
      AddNewNode<StoreMap>(
          {new_alloc},
          MakeRefAssumeMemoryFence(
              broker(), local_isolate()->factory()->heap_number_map()));
      // TODO(leszeks): Fix hole storage, in case this should be a custom NaN.
      AddNewNode<StoreFloat64>(
          {new_alloc, GetFloat64Constant(value.mutable_double_value)},
          HeapNumber::kValueOffset);
      EnsureType(new_alloc, NodeType::kNumber);
      return new_alloc;
    }

    case FastField::kConstant:
      return GetConstant(value.constant_value);
    case FastField::kUninitialized:
      return GetRootConstant(RootIndex::kOnePointerFillerMap);
  }
}

ValueNode* MaglevGraphBuilder::BuildAllocateFastObject(
    FastFixedArray value, AllocationType allocation_type) {
  switch (value.type) {
    case FastFixedArray::kTagged: {
      SmallZoneVector<ValueNode*, 8> elements(value.length, zone());
      for (int i = 0; i < value.length; ++i) {
        elements[i] = BuildAllocateFastObject(value.values[i], allocation_type);
      }
      ValueNode* allocation = ExtendOrReallocateCurrentRawAllocation(
          FixedArray::SizeFor(value.length), allocation_type);
      AddNewNode<StoreMap>(
          {allocation},
          MakeRefAssumeMemoryFence(
              broker(), local_isolate()->factory()->fixed_array_map()));
      AddNewNode<StoreTaggedFieldNoWriteBarrier>(
          {allocation, GetSmiConstant(value.length)},
          FixedArray::kLengthOffset);
      for (int i = 0; i < value.length; ++i) {
        // TODO(leszeks): Elide the write barrier where possible.
        BuildStoreTaggedField(allocation, elements[i],
                              FixedArray::OffsetOfElementAt(i));
      }
      EnsureType(allocation, NodeType::kJSReceiver);
      return allocation;
    }
    case FastFixedArray::kDouble: {
      ValueNode* allocation = ExtendOrReallocateCurrentRawAllocation(
          FixedDoubleArray::SizeFor(value.length), allocation_type);
      AddNewNode<StoreMap>(
          {allocation},
          MakeRefAssumeMemoryFence(
              broker(), local_isolate()->factory()->fixed_double_array_map()));
      AddNewNode<StoreTaggedFieldNoWriteBarrier>(
          {allocation, GetSmiConstant(value.length)},
          FixedDoubleArray::kLengthOffset);
      for (int i = 0; i < value.length; ++i) {
        // TODO(leszeks): Fix hole storage, in case Float64::get_scalar doesn't
        // preserve custom NaNs.
        AddNewNode<StoreFloat64>(
            {allocation, GetFloat64Constant(value.double_values[i])},
            FixedDoubleArray::OffsetOfElementAt(i));
      }
      return allocation;
    }
    case FastFixedArray::kCoW:
      return GetConstant(value.cow_value);
    case FastFixedArray::kUninitialized:
      return GetRootConstant(RootIndex::kEmptyFixedArray);
  }
}

ReduceResult MaglevGraphBuilder::TryBuildFastCreateObjectOrArrayLiteral(
    const compiler::LiteralFeedback& feedback) {
  compiler::AllocationSiteRef site = feedback.value();
  if (!site.boilerplate(broker()).has_value()) return ReduceResult::Fail();
  AllocationType allocation_type =
      broker()->dependencies()->DependOnPretenureMode(site);

  // First try to extract out the shape and values of the boilerplate, bailing
  // out on complex boilerplates.
  int max_properties = compiler::kMaxFastLiteralProperties;
  base::Optional<FastObject> maybe_value = TryReadBoilerplateForFastLiteral(
      *site.boilerplate(broker()), allocation_type,
      compiler::kMaxFastLiteralDepth, &max_properties);
  if (!maybe_value.has_value()) return ReduceResult::Fail();

  // Then, use the collected information to actually create nodes in the graph.
  // TODO(leszeks): Add support for unwinding graph modifications, so that we
  // can get rid of this two pass approach.
  broker()->dependencies()->DependOnElementsKinds(site);
  ReduceResult result = BuildAllocateFastObject(*maybe_value, allocation_type);
  // TODO(leszeks): Don't eagerly clear the raw allocation, have the next side
  // effect clear it.
  ClearCurrentRawAllocation();
  return result;
}

void MaglevGraphBuilder::VisitCreateObjectLiteral() {
  compiler::ObjectBoilerplateDescriptionRef boilerplate_desc =
      GetRefOperand<ObjectBoilerplateDescription>(0);
  FeedbackSlot slot_index = GetSlotOperand(1);
  int bytecode_flags = GetFlag8Operand(2);
  int literal_flags =
      interpreter::CreateObjectLiteralFlags::FlagsBits::decode(bytecode_flags);
  compiler::FeedbackSource feedback_source(feedback(), slot_index);

  compiler::ProcessedFeedback const& processed_feedback =
      broker()->GetFeedbackForArrayOrObjectLiteral(feedback_source);
  if (processed_feedback.IsInsufficient()) {
    RETURN_VOID_ON_ABORT(EmitUnconditionalDeopt(
        DeoptimizeReason::kInsufficientTypeFeedbackForObjectLiteral));
  }

  ReduceResult result =
      TryBuildFastCreateObjectOrArrayLiteral(processed_feedback.AsLiteral());
  PROCESS_AND_RETURN_IF_DONE(result, SetAccumulator);

  if (interpreter::CreateObjectLiteralFlags::FastCloneSupportedBit::decode(
          bytecode_flags)) {
    // TODO(victorgomes): CreateShallowObjectLiteral should not need the
    // boilerplate descriptor. However the current builtin checks that the
    // feedback exists and fallsback to CreateObjectLiteral if it doesn't.
    SetAccumulator(AddNewNode<CreateShallowObjectLiteral>(
        {}, boilerplate_desc, feedback_source, literal_flags));
  } else {
    SetAccumulator(AddNewNode<CreateObjectLiteral>(
        {}, boilerplate_desc, feedback_source, literal_flags));
  }
}

void MaglevGraphBuilder::VisitCreateEmptyObjectLiteral() {
  compiler::NativeContextRef native_context = broker()->target_native_context();
  compiler::MapRef map =
      native_context.object_function(broker()).initial_map(broker());
  DCHECK(!map.is_dictionary_map());
  DCHECK(!map.IsInobjectSlackTrackingInProgress());
  FastObject literal(map, zone(), {});
  literal.ClearFields();
  SetAccumulator(BuildAllocateFastObject(literal, AllocationType::kYoung));
  // TODO(leszeks): Don't eagerly clear the raw allocation, have the next side
  // effect clear it.
  ClearCurrentRawAllocation();
}

void MaglevGraphBuilder::VisitCloneObject() {
  // CloneObject <source_idx> <flags> <feedback_slot>
  ValueNode* source = LoadRegisterTagged(0);
  ValueNode* flags =
      GetSmiConstant(interpreter::CreateObjectLiteralFlags::FlagsBits::decode(
          GetFlag8Operand(1)));
  FeedbackSlot slot = GetSlotOperand(2);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  SetAccumulator(BuildCallBuiltin<Builtin::kCloneObjectIC>({source, flags},
                                                           feedback_source));
}

void MaglevGraphBuilder::VisitGetTemplateObject() {
  // GetTemplateObject <descriptor_idx> <literal_idx>
  compiler::SharedFunctionInfoRef shared_function_info =
      compilation_unit_->shared_function_info();
  ValueNode* description = GetConstant(GetRefOperand<HeapObject>(0));
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  const compiler::ProcessedFeedback& feedback =
      broker()->GetFeedbackForTemplateObject(feedback_source);
  if (feedback.IsInsufficient()) {
    return SetAccumulator(AddNewNode<GetTemplateObject>(
        {description}, shared_function_info, feedback_source));
  }
  compiler::JSArrayRef template_object = feedback.AsTemplateObject().value();
  SetAccumulator(GetConstant(template_object));
}

void MaglevGraphBuilder::VisitCreateClosure() {
  compiler::SharedFunctionInfoRef shared_function_info =
      GetRefOperand<SharedFunctionInfo>(0);
  compiler::FeedbackCellRef feedback_cell =
      feedback().GetClosureFeedbackCell(broker(), iterator_.GetIndexOperand(1));
  uint32_t flags = GetFlag8Operand(2);

  if (interpreter::CreateClosureFlags::FastNewClosureBit::decode(flags)) {
    SetAccumulator(AddNewNode<FastCreateClosure>(
        {GetContext()}, shared_function_info, feedback_cell));
  } else {
    bool pretenured =
        interpreter::CreateClosureFlags::PretenuredBit::decode(flags);
    SetAccumulator(AddNewNode<CreateClosure>(
        {GetContext()}, shared_function_info, feedback_cell, pretenured));
  }
}

void MaglevGraphBuilder::VisitCreateBlockContext() {
  // TODO(v8:7700): Inline allocation when context is small.
  // TODO(v8:7700): Update TryGetParentContext if this ever emits its own Node
  // type.
  // CreateBlockContext <scope_info_idx>
  ValueNode* scope_info = GetConstant(GetRefOperand<ScopeInfo>(0));
  SetAccumulator(BuildCallRuntime(Runtime::kPushBlockContext, {scope_info}));
}

void MaglevGraphBuilder::VisitCreateCatchContext() {
  // TODO(v8:7700): Inline allocation when context is small.
  // TODO(v8:7700): Update TryGetParentContext if this ever emits its own Node
  // type.
  // CreateCatchContext <exception> <scope_info_idx>
  ValueNode* exception = LoadRegisterTagged(0);
  ValueNode* scope_info = GetConstant(GetRefOperand<ScopeInfo>(1));
  SetAccumulator(
      BuildCallRuntime(Runtime::kPushCatchContext, {exception, scope_info}));
}

void MaglevGraphBuilder::VisitCreateFunctionContext() {
  compiler::ScopeInfoRef info = GetRefOperand<ScopeInfo>(0);
  uint32_t slot_count = iterator_.GetUnsignedImmediateOperand(1);
  SetAccumulator(AddNewNode<CreateFunctionContext>(
      {GetContext()}, info, slot_count, ScopeType::FUNCTION_SCOPE));
}

void MaglevGraphBuilder::VisitCreateEvalContext() {
  // TODO(v8:7700): Update TryGetParentContext if this ever emits its own Node
  // type.
  compiler::ScopeInfoRef info = GetRefOperand<ScopeInfo>(0);
  uint32_t slot_count = iterator_.GetUnsignedImmediateOperand(1);
  if (slot_count <= static_cast<uint32_t>(
                        ConstructorBuiltins::MaximumFunctionContextSlots())) {
    SetAccumulator(AddNewNode<CreateFunctionContext>(
        {GetContext()}, info, slot_count, ScopeType::EVAL_SCOPE));
  } else {
    SetAccumulator(
        BuildCallRuntime(Runtime::kNewFunctionContext, {GetConstant(info)}));
  }
}

void MaglevGraphBuilder::VisitCreateWithContext() {
  // TODO(v8:7700): Inline allocation when context is small.
  // CreateWithContext <register> <scope_info_idx>
  ValueNode* object = LoadRegisterTagged(0);
  ValueNode* scope_info = GetConstant(GetRefOperand<ScopeInfo>(1));
  SetAccumulator(
      BuildCallRuntime(Runtime::kPushWithContext, {object, scope_info}));
}

void MaglevGraphBuilder::VisitCreateMappedArguments() {
  compiler::SharedFunctionInfoRef shared =
      compilation_unit_->shared_function_info();
  if (is_inline() || shared.object()->has_duplicate_parameters()) {
    SetAccumulator(
        BuildCallRuntime(Runtime::kNewSloppyArguments, {GetClosure()}));
  } else {
    SetAccumulator(
        BuildCallBuiltin<Builtin::kFastNewSloppyArguments>({GetClosure()}));
  }
}

void MaglevGraphBuilder::VisitCreateUnmappedArguments() {
  if (is_inline()) {
    SetAccumulator(
        BuildCallRuntime(Runtime::kNewStrictArguments, {GetClosure()}));
  } else {
    SetAccumulator(
        BuildCallBuiltin<Builtin::kFastNewStrictArguments>({GetClosure()}));
  }
}

void MaglevGraphBuilder::VisitCreateRestParameter() {
  if (is_inline()) {
    SetAccumulator(
        BuildCallRuntime(Runtime::kNewRestParameter, {GetClosure()}));
  } else {
    SetAccumulator(
        BuildCallBuiltin<Builtin::kFastNewRestArguments>({GetClosure()}));
  }
}

void MaglevGraphBuilder::PeelLoop() {
  DCHECK(!in_peeled_iteration_);
  int loop_header = iterator_.current_offset();
  DCHECK(loop_headers_to_peel_.Contains(loop_header));
  in_peeled_iteration_ = true;
  allow_loop_peeling_ = false;
  while (iterator_.current_bytecode() != interpreter::Bytecode::kJumpLoop) {
    local_isolate_->heap()->Safepoint();
    VisitSingleBytecode();
    iterator_.Advance();
  }
  VisitSingleBytecode();
  in_peeled_iteration_ = false;

  // After processing the peeled iteration and reaching the `JumpLoop`, we
  // re-process the loop body. For this, we need to reset the graph building
  // state roughly as if we didn't process it yet.

  // Reset predecessors as if the loop body had not been visited.
  while (!decremented_predecessor_offsets_.empty()) {
    DCHECK_GE(decremented_predecessor_offsets_.back(), loop_header);
    if (decremented_predecessor_offsets_.back() <= iterator_.current_offset()) {
      predecessors_[decremented_predecessor_offsets_.back()]++;
    }
    decremented_predecessor_offsets_.pop_back();
  }

  // Reset position in exception handler table to before the loop.
  HandlerTable table(*bytecode().object());
  while (next_handler_table_index_ > 0) {
    next_handler_table_index_--;
    int start = table.GetRangeStart(next_handler_table_index_);
    if (start < loop_header) break;
  }

  // Re-create catch handler merge states.
  for (int offset = loop_header; offset <= iterator_.current_offset();
       ++offset) {
    if (auto& merge_state = merge_states_[offset]) {
      if (merge_state->is_exception_handler()) {
        merge_state = MergePointInterpreterFrameState::NewForCatchBlock(
            *compilation_unit_, merge_state->frame_state().liveness(), offset,
            merge_state->catch_block_context_register(), graph_);
      } else {
        // We only peel innermost loops.
        DCHECK(!merge_state->is_loop());
        merge_state = nullptr;
      }
    }
    new (&jump_targets_[offset]) BasicBlockRef();
  }

  if (current_block_) {
    // After resetting, the new loop header always has exactly 2 predecessors:
    // the two copies of `JumpLoop`.
    merge_states_[loop_header] = MergePointInterpreterFrameState::NewForLoop(
        current_interpreter_frame_, *compilation_unit_, loop_header, 2,
        GetInLivenessFor(loop_header),
        &bytecode_analysis_.GetLoopInfoFor(loop_header),
        /* has_been_peeled */ true);

    BasicBlock* block = FinishBlock<Jump>({}, &jump_targets_[loop_header]);
    MergeIntoFrameState(block, loop_header);
  } else {
    merge_states_[loop_header] = nullptr;
    predecessors_[loop_header] = 0;
  }
  iterator_.SetOffset(loop_header);
}

void MaglevGraphBuilder::VisitJumpLoop() {
  const uint32_t relative_jump_bytecode_offset =
      iterator_.GetUnsignedImmediateOperand(0);
  const int32_t loop_offset = iterator_.GetImmediateOperand(1);
  const FeedbackSlot feedback_slot = iterator_.GetSlotOperand(2);
  int target = iterator_.GetJumpTargetOffset();

  if (ShouldEmitInterruptBudgetChecks()) {
    AddNewNode<ReduceInterruptBudgetForLoop>(
        {}, relative_jump_bytecode_offset ? relative_jump_bytecode_offset : 1);
  } else {
    AddNewNode<HandleNoHeapWritesInterrupt>({});
  }

  if (in_peeled_iteration_) {
    // We have reached the end of the peeled iteration.
    return;
  }

  if (ShouldEmitOsrInterruptBudgetChecks()) {
    AddNewNode<TryOnStackReplacement>(
        {GetClosure()}, loop_offset, feedback_slot,
        BytecodeOffset(iterator_.current_offset()), compilation_unit_);
  }
  BasicBlock* block =
      FinishBlock<JumpLoop>({}, jump_targets_[target].block_ptr());

  merge_states_[target]->MergeLoop(this, current_interpreter_frame_, block);
  block->set_predecessor_id(merge_states_[target]->predecessor_count() - 1);
  if (loop_headers_to_peel_.Contains(iterator_.current_offset())) {
    allow_loop_peeling_ = true;
  }
}
void MaglevGraphBuilder::VisitJump() {
  BasicBlock* block =
      FinishBlock<Jump>({}, &jump_targets_[iterator_.GetJumpTargetOffset()]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
  DCHECK_EQ(current_block_, nullptr);
  DCHECK_LT(next_offset(), bytecode().length());
}
void MaglevGraphBuilder::VisitJumpConstant() { VisitJump(); }
void MaglevGraphBuilder::VisitJumpIfNullConstant() { VisitJumpIfNull(); }
void MaglevGraphBuilder::VisitJumpIfNotNullConstant() { VisitJumpIfNotNull(); }
void MaglevGraphBuilder::VisitJumpIfUndefinedConstant() {
  VisitJumpIfUndefined();
}
void MaglevGraphBuilder::VisitJumpIfNotUndefinedConstant() {
  VisitJumpIfNotUndefined();
}
void MaglevGraphBuilder::VisitJumpIfUndefinedOrNullConstant() {
  VisitJumpIfUndefinedOrNull();
}
void MaglevGraphBuilder::VisitJumpIfTrueConstant() { VisitJumpIfTrue(); }
void MaglevGraphBuilder::VisitJumpIfFalseConstant() { VisitJumpIfFalse(); }
void MaglevGraphBuilder::VisitJumpIfJSReceiverConstant() {
  VisitJumpIfJSReceiver();
}
void MaglevGraphBuilder::VisitJumpIfToBooleanTrueConstant() {
  VisitJumpIfToBooleanTrue();
}
void MaglevGraphBuilder::VisitJumpIfToBooleanFalseConstant() {
  VisitJumpIfToBooleanFalse();
}

void MaglevGraphBuilder::MergeIntoFrameState(BasicBlock* predecessor,
                                             int target) {
  if (merge_states_[target] == nullptr) {
    DCHECK(!bytecode_analysis().IsLoopHeader(target) ||
           loop_headers_to_peel_.Contains(target));
    bool jumping_to_peeled_iteration = bytecode_analysis().IsLoopHeader(target);
    const compiler::BytecodeLivenessState* liveness = GetInLivenessFor(target);
    int num_of_predecessors = NumPredecessors(target);
    if (jumping_to_peeled_iteration) {
      // The peeled iteration is missing the backedge.
      num_of_predecessors--;
    }
    // If there's no target frame state, allocate a new one.
    merge_states_[target] = MergePointInterpreterFrameState::New(
        *compilation_unit_, current_interpreter_frame_, target,
        num_of_predecessors, predecessor, liveness);
  } else {
    // If there already is a frame state, merge.
    merge_states_[target]->Merge(this, current_interpreter_frame_, predecessor);
  }
}

void MaglevGraphBuilder::MergeDeadIntoFrameState(int target) {
  // If there is no merge state yet, don't create one, but just reduce the
  // number of possible predecessors to zero.
  predecessors_[target]--;
  if (V8_UNLIKELY(in_peeled_iteration_)) {
    decremented_predecessor_offsets_.push_back(target);
  }
  if (merge_states_[target]) {
    // If there already is a frame state, merge.
    merge_states_[target]->MergeDead(*compilation_unit_);
    // If this merge is the last one which kills a loop merge, remove that
    // merge state.
    if (merge_states_[target]->is_unreachable_loop()) {
      if (v8_flags.trace_maglev_graph_building) {
        std::cout << "! Killing loop merge state at @" << target << std::endl;
      }
      merge_states_[target] = nullptr;
    }
  }
}

void MaglevGraphBuilder::MergeDeadLoopIntoFrameState(int target) {
  // Check if the Loop entry is dead (e.g. an outer loop from OSR).
  if (V8_UNLIKELY(!merge_states_[target])) {
    static_assert(kLoopsMustBeEnteredThroughHeader);
    return;
  }
  // If there is no merge state yet, don't create one, but just reduce the
  // number of possible predecessors to zero.
  predecessors_[target]--;
  if (V8_UNLIKELY(in_peeled_iteration_)) {
    decremented_predecessor_offsets_.push_back(target);
  }
  if (merge_states_[target]->is_unreachable_loop()) {
    static_assert(MaglevGraphBuilder::kLoopsMustBeEnteredThroughHeader);
  } else {
    // If there already is a frame state, merge.
    merge_states_[target]->MergeDeadLoop(*compilation_unit_);
  }
}

void MaglevGraphBuilder::MergeIntoInlinedReturnFrameState(
    BasicBlock* predecessor) {
  int target = inline_exit_offset();
  if (merge_states_[target] == nullptr) {
    // All returns should have the same liveness, which is that only the
    // accumulator is live.
    const compiler::BytecodeLivenessState* liveness = GetInLiveness();
    DCHECK(liveness->AccumulatorIsLive());
    DCHECK_EQ(liveness->live_value_count(), 1);

    // If there's no target frame state, allocate a new one.
    merge_states_[target] = MergePointInterpreterFrameState::New(
        *compilation_unit_, current_interpreter_frame_, target,
        NumPredecessors(target), predecessor, liveness);
  } else {
    // Again, all returns should have the same liveness, so double check this.
    DCHECK(GetInLiveness()->Equals(
        *merge_states_[target]->frame_state().liveness()));
    merge_states_[target]->Merge(this, current_interpreter_frame_, predecessor);
  }
}

MaglevGraphBuilder::JumpType MaglevGraphBuilder::NegateJumpType(
    JumpType jump_type) {
  switch (jump_type) {
    case JumpType::kJumpIfTrue:
      return JumpType::kJumpIfFalse;
    case JumpType::kJumpIfFalse:
      return JumpType::kJumpIfTrue;
  }
}

void MaglevGraphBuilder::BuildBranchIfRootConstant(
    ValueNode* node, JumpType jump_type, RootIndex root_index,
    BranchSpecializationMode mode) {
  ValueNode* original_node = node;
  JumpType original_jump_type = jump_type;

  int fallthrough_offset = next_offset();
  int jump_offset = iterator_.GetJumpTargetOffset();

  BasicBlockRef *true_target, *false_target;
  if (jump_type == kJumpIfTrue) {
    true_target = &jump_targets_[jump_offset];
    false_target = &jump_targets_[fallthrough_offset];
  } else {
    true_target = &jump_targets_[fallthrough_offset];
    false_target = &jump_targets_[jump_offset];
  }

  if (root_index != RootIndex::kTrueValue &&
      root_index != RootIndex::kFalseValue &&
      CheckType(node, NodeType::kBoolean)) {
    bool is_jump_taken = jump_type == kJumpIfFalse;
    if (is_jump_taken) {
      BasicBlock* block = FinishBlock<Jump>({}, &jump_targets_[jump_offset]);
      MergeDeadIntoFrameState(fallthrough_offset);
      MergeIntoFrameState(block, jump_offset);
    } else {
      MergeDeadIntoFrameState(jump_offset);
    }
    return;
  }

  while (LogicalNot* logical_not = node->TryCast<LogicalNot>()) {
    // Bypassing logical not(s) on the input and swapping true/false
    // destinations.
    node = logical_not->value().node();
    std::swap(true_target, false_target);
    jump_type = NegateJumpType(jump_type);
  }

  if (RootConstant* c = node->TryCast<RootConstant>()) {
    bool constant_is_match = c->index() == root_index;
    bool is_jump_taken = constant_is_match == (jump_type == kJumpIfTrue);
    if (is_jump_taken) {
      BasicBlock* block = FinishBlock<Jump>({}, &jump_targets_[jump_offset]);
      MergeDeadIntoFrameState(fallthrough_offset);
      MergeIntoFrameState(block, jump_offset);
    } else {
      MergeDeadIntoFrameState(jump_offset);
    }
    return;
  }

  BasicBlock* block = FinishBlock<BranchIfRootConstant>(
      {node}, root_index, true_target, false_target);

  // If the node we're checking is in the accumulator, swap it in the branch
  // with the checked value. Cache whether we want to swap, since after we've
  // swapped the accumulator isn't the original node anymore.
  bool swap_accumulator = original_node == GetRawAccumulator();

  if (swap_accumulator) {
    if (mode == BranchSpecializationMode::kAlwaysBoolean) {
      SetAccumulatorInBranch(
          GetBooleanConstant(original_jump_type == kJumpIfTrue));
    } else if (original_jump_type == kJumpIfTrue) {
      SetAccumulatorInBranch(GetRootConstant(root_index));
    } else {
      SetAccumulatorInBranch(node);
    }
  }

  MergeIntoFrameState(block, jump_offset);

  if (swap_accumulator) {
    if (mode == BranchSpecializationMode::kAlwaysBoolean) {
      SetAccumulatorInBranch(
          GetBooleanConstant(original_jump_type == kJumpIfFalse));
    } else if (original_jump_type == kJumpIfFalse) {
      SetAccumulatorInBranch(GetRootConstant(root_index));
    } else {
      SetAccumulatorInBranch(node);
    }
  }

  StartFallthroughBlock(fallthrough_offset, block);
}
void MaglevGraphBuilder::BuildBranchIfTrue(ValueNode* node,
                                           JumpType jump_type) {
  BuildBranchIfRootConstant(node, jump_type, RootIndex::kTrueValue,
                            BranchSpecializationMode::kAlwaysBoolean);
}
void MaglevGraphBuilder::BuildBranchIfNull(ValueNode* node,
                                           JumpType jump_type) {
  BuildBranchIfRootConstant(node, jump_type, RootIndex::kNullValue);
}
void MaglevGraphBuilder::BuildBranchIfUndefined(ValueNode* node,
                                                JumpType jump_type) {
  BuildBranchIfRootConstant(node, jump_type, RootIndex::kUndefinedValue);
}

BasicBlock* MaglevGraphBuilder::BuildSpecializedBranchIfCompareNode(
    ValueNode* node, BasicBlockRef* true_target, BasicBlockRef* false_target) {
  auto make_specialized_branch_if_compare = [&](ValueRepresentation repr,
                                                ValueNode* cond) {
    // Note that this function (BuildBranchIfToBooleanTrue) generates either a
    // BranchIfToBooleanTrue, or a BranchIfFloat64Compare/BranchIfInt32Compare
    // comparing the {node} with 0. In the former case, the jump is taken if
    // {node} is non-zero, while in the latter cases, the jump is taken if
    // {node} is zero. The {true_target} and {false_target} are thus swapped
    // when we generate a BranchIfFloat64Compare or a BranchIfInt32Compare
    // below.
    switch (repr) {
      case ValueRepresentation::kFloat64:
      case ValueRepresentation::kHoleyFloat64:
        DCHECK(cond->value_representation() == ValueRepresentation::kFloat64 ||
               cond->value_representation() ==
                   ValueRepresentation::kHoleyFloat64);
        // The ToBoolean of both the_hole and NaN is false, so we can use the
        // same operation for HoleyFloat64 and Float64.
        return FinishBlock<BranchIfFloat64ToBooleanTrue>({cond}, true_target,
                                                         false_target);

      case ValueRepresentation::kInt32:
        DCHECK_EQ(cond->value_representation(), ValueRepresentation::kInt32);
        return FinishBlock<BranchIfInt32ToBooleanTrue>({cond}, true_target,
                                                       false_target);
      default:
        UNREACHABLE();
    }
  };

  if (node->value_representation() == ValueRepresentation::kInt32) {
    return make_specialized_branch_if_compare(ValueRepresentation::kInt32,
                                              node);
  } else if (node->value_representation() == ValueRepresentation::kFloat64 ||
             node->value_representation() ==
                 ValueRepresentation::kHoleyFloat64) {
    return make_specialized_branch_if_compare(ValueRepresentation::kFloat64,
                                              node);
  } else {
    NodeInfo* node_info = known_node_aspects().GetOrCreateInfoFor(node);
    if (auto as_int32 = node_info->alternative().int32()) {
      return make_specialized_branch_if_compare(ValueRepresentation::kInt32,
                                                as_int32);
    } else if (auto as_float64 = node_info->alternative().float64()) {
      return make_specialized_branch_if_compare(ValueRepresentation::kFloat64,
                                                as_float64);
    } else {
      DCHECK(node->value_representation() == ValueRepresentation::kTagged ||
             node->value_representation() == ValueRepresentation::kUint32);
      // Uint32 should be rare enough that tagging them shouldn't be too
      // expensive (we don't have a `BranchIfUint32Compare` node, and adding
      // one doesn't seem worth it at this point).
      // We do not record a Tagged use hint here, because Tagged hints prevent
      // Phi untagging. A BranchIfToBooleanTrue should never prevent Phi
      // untagging: if we untag a Phi that is used in such a node, then we can
      // convert the node to BranchIfFloat64ToBooleanTrue or
      // BranchIfInt32ToBooleanTrue.
      node = GetTaggedValue(node, UseReprHintRecording::kDoNotRecord);
      NodeType old_type;
      if (CheckType(node, NodeType::kBoolean, &old_type)) {
        return FinishBlock<BranchIfRootConstant>({node}, RootIndex::kTrueValue,
                                                 true_target, false_target);
      }
      if (CheckType(node, NodeType::kSmi)) {
        return FinishBlock<BranchIfReferenceCompare>({node, GetSmiConstant(0)},
                                                     Operation::kStrictEqual,
                                                     false_target, true_target);
      }
      if (CheckType(node, NodeType::kString)) {
        return FinishBlock<BranchIfRootConstant>(
            {node}, RootIndex::kempty_string, false_target, true_target);
      }
      // TODO(verwaest): Number or oddball.
      return FinishBlock<BranchIfToBooleanTrue>({node}, GetCheckType(old_type),
                                                true_target, false_target);
    }
  }
}

void MaglevGraphBuilder::BuildBranchIfToBooleanTrue(ValueNode* node,
                                                    JumpType jump_type) {
  int fallthrough_offset = next_offset();
  int jump_offset = iterator_.GetJumpTargetOffset();

  BasicBlockRef *true_target, *false_target;
  if (jump_type == kJumpIfTrue) {
    true_target = &jump_targets_[jump_offset];
    false_target = &jump_targets_[fallthrough_offset];
  } else {
    true_target = &jump_targets_[fallthrough_offset];
    false_target = &jump_targets_[jump_offset];
  }

  while (LogicalNot* logical_not = node->TryCast<LogicalNot>()) {
    // Bypassing logical not(s) on the input and swapping true/false
    // destinations.
    node = logical_not->value().node();
    std::swap(true_target, false_target);
    jump_type = NegateJumpType(jump_type);
  }

  bool known_to_boolean_value = false;
  bool direction_is_true = true;
  if (IsConstantNode(node->opcode())) {
    known_to_boolean_value = true;
    direction_is_true = FromConstantToBool(local_isolate(), node);
  } else if (CheckType(node, NodeType::kJSReceiver)) {
    known_to_boolean_value = true;
    direction_is_true = true;
  }
  if (known_to_boolean_value) {
    bool is_jump_taken = direction_is_true == (jump_type == kJumpIfTrue);
    if (is_jump_taken) {
      BasicBlock* block = FinishBlock<Jump>({}, &jump_targets_[jump_offset]);
      MergeDeadIntoFrameState(fallthrough_offset);
      MergeIntoFrameState(block, jump_offset);
    } else {
      MergeDeadIntoFrameState(jump_offset);
    }
    return;
  }

  BasicBlock* block =
      BuildSpecializedBranchIfCompareNode(node, true_target, false_target);

  MergeIntoFrameState(block, jump_offset);
  StartFallthroughBlock(fallthrough_offset, block);
}
void MaglevGraphBuilder::VisitJumpIfToBooleanTrue() {
  BuildBranchIfToBooleanTrue(GetRawAccumulator(), kJumpIfTrue);
}
void MaglevGraphBuilder::VisitJumpIfToBooleanFalse() {
  BuildBranchIfToBooleanTrue(GetRawAccumulator(), kJumpIfFalse);
}
void MaglevGraphBuilder::VisitJumpIfTrue() {
  BuildBranchIfTrue(GetAccumulatorTagged(), kJumpIfTrue);
}
void MaglevGraphBuilder::VisitJumpIfFalse() {
  BuildBranchIfTrue(GetAccumulatorTagged(), kJumpIfFalse);
}
void MaglevGraphBuilder::VisitJumpIfNull() {
  BuildBranchIfNull(GetAccumulatorTagged(), kJumpIfTrue);
}
void MaglevGraphBuilder::VisitJumpIfNotNull() {
  BuildBranchIfNull(GetAccumulatorTagged(), kJumpIfFalse);
}
void MaglevGraphBuilder::VisitJumpIfUndefined() {
  BuildBranchIfUndefined(GetAccumulatorTagged(), kJumpIfTrue);
}
void MaglevGraphBuilder::VisitJumpIfNotUndefined() {
  BuildBranchIfUndefined(GetAccumulatorTagged(), kJumpIfFalse);
}
void MaglevGraphBuilder::VisitJumpIfUndefinedOrNull() {
  BasicBlock* block = FinishBlock<BranchIfUndefinedOrNull>(
      {GetAccumulatorTagged()}, &jump_targets_[iterator_.GetJumpTargetOffset()],
      &jump_targets_[next_offset()]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
  StartFallthroughBlock(next_offset(), block);
}
void MaglevGraphBuilder::VisitJumpIfJSReceiver() {
  BasicBlock* block = FinishBlock<BranchIfJSReceiver>(
      {GetAccumulatorTagged()}, &jump_targets_[iterator_.GetJumpTargetOffset()],
      &jump_targets_[next_offset()]);
  MergeIntoFrameState(block, iterator_.GetJumpTargetOffset());
  StartFallthroughBlock(next_offset(), block);
}

void MaglevGraphBuilder::VisitSwitchOnSmiNoFeedback() {
  // SwitchOnSmiNoFeedback <table_start> <table_length> <case_value_base>
  interpreter::JumpTableTargetOffsets offsets =
      iterator_.GetJumpTableTargetOffsets();

  if (offsets.size() == 0) return;

  int case_value_base = (*offsets.begin()).case_value;
  BasicBlockRef* targets = zone()->AllocateArray<BasicBlockRef>(offsets.size());
  for (interpreter::JumpTableTargetOffset offset : offsets) {
    BasicBlockRef* ref = &targets[offset.case_value - case_value_base];
    new (ref) BasicBlockRef(&jump_targets_[offset.target_offset]);
  }

  ValueNode* case_value = GetAccumulatorInt32();
  BasicBlock* block =
      FinishBlock<Switch>({case_value}, case_value_base, targets,
                          offsets.size(), &jump_targets_[next_offset()]);
  for (interpreter::JumpTableTargetOffset offset : offsets) {
    MergeIntoFrameState(block, offset.target_offset);
  }
  StartFallthroughBlock(next_offset(), block);
}

void MaglevGraphBuilder::VisitForInEnumerate() {
  // ForInEnumerate <receiver>
  ValueNode* receiver = LoadRegisterTagged(0);
  SetAccumulator(BuildCallBuiltin<Builtin::kForInEnumerate>({receiver}));
}

void MaglevGraphBuilder::VisitForInPrepare() {
  // ForInPrepare <cache_info_triple>
  ValueNode* enumerator = GetAccumulatorTagged();
  FeedbackSlot slot = GetSlotOperand(1);
  compiler::FeedbackSource feedback_source{feedback(), slot};
  // TODO(v8:7700): Use feedback and create fast path.
  ValueNode* context = GetContext();
  interpreter::Register cache_type_reg = iterator_.GetRegisterOperand(0);
  interpreter::Register cache_array_reg{cache_type_reg.index() + 1};
  interpreter::Register cache_length_reg{cache_type_reg.index() + 2};

  ForInHint hint = broker()->GetFeedbackForForIn(feedback_source);

  current_for_in_state = ForInState();
  switch (hint) {
    case ForInHint::kNone:
    case ForInHint::kEnumCacheKeysAndIndices:
    case ForInHint::kEnumCacheKeys: {
      RETURN_VOID_IF_ABORT(
          BuildCheckMaps(enumerator, base::VectorOf({broker()->meta_map()})));

      auto* descriptor_array = AddNewNode<LoadTaggedField>(
          {enumerator}, Map::kInstanceDescriptorsOffset);
      auto* enum_cache = AddNewNode<LoadTaggedField>(
          {descriptor_array}, DescriptorArray::kEnumCacheOffset);
      auto* cache_array =
          AddNewNode<LoadTaggedField>({enum_cache}, EnumCache::kKeysOffset);
      current_for_in_state.enum_cache = enum_cache;

      auto* cache_length = AddNewNode<LoadEnumCacheLength>({enumerator});

      MoveNodeBetweenRegisters(interpreter::Register::virtual_accumulator(),
                               cache_type_reg);
      StoreRegister(cache_array_reg, cache_array);
      StoreRegister(cache_length_reg, cache_length);
      break;
    }
    case ForInHint::kAny: {
      // The result of the  bytecode is output in registers |cache_info_triple|
      // to |cache_info_triple + 2|, with the registers holding cache_type,
      // cache_array, and cache_length respectively.
      //
      // We set the cache type first (to the accumulator value), and write
      // the other two with a ForInPrepare builtin call. This can lazy deopt,
      // which will write to cache_array and cache_length, with cache_type
      // already set on the translation frame.

      // This move needs to happen before ForInPrepare to avoid lazy deopt
      // extending the lifetime of the {cache_type} register.
      MoveNodeBetweenRegisters(interpreter::Register::virtual_accumulator(),
                               cache_type_reg);
      ForInPrepare* result =
          AddNewNode<ForInPrepare>({context, enumerator}, feedback_source);
      // The add will set up a lazy deopt info writing to all three output
      // registers. Update this to only write to the latter two.
      result->lazy_deopt_info()->UpdateResultLocation(cache_array_reg, 2);
      StoreRegisterPair({cache_array_reg, cache_length_reg}, result);
      // Force a conversion to Int32 for the cache length value.
      GetInt32(cache_length_reg);
      break;
    }
  }
}

void MaglevGraphBuilder::VisitForInContinue() {
  // ForInContinue <index> <cache_length>
  ValueNode* index = LoadRegisterInt32(0);
  ValueNode* cache_length = LoadRegisterInt32(1);
  if (TryBuildBranchFor<BranchIfInt32Compare>({index, cache_length},
                                              Operation::kLessThan)) {
    return;
  }
  SetAccumulator(
      AddNewNode<Int32NodeFor<Operation::kLessThan>>({index, cache_length}));
}

void MaglevGraphBuilder::VisitForInNext() {
  // ForInNext <receiver> <index> <cache_info_pair>
  ValueNode* receiver = LoadRegisterTagged(0);
  interpreter::Register cache_type_reg, cache_array_reg;
  std::tie(cache_type_reg, cache_array_reg) =
      iterator_.GetRegisterPairOperand(2);
  ValueNode* cache_type = GetTaggedValue(cache_type_reg);
  ValueNode* cache_array = GetTaggedValue(cache_array_reg);
  FeedbackSlot slot = GetSlotOperand(3);
  compiler::FeedbackSource feedback_source{feedback(), slot};

  ForInHint hint = broker()->GetFeedbackForForIn(feedback_source);

  switch (hint) {
    case ForInHint::kNone:
    case ForInHint::kEnumCacheKeysAndIndices:
    case ForInHint::kEnumCacheKeys: {
      ValueNode* index = LoadRegisterInt32(1);
      // Ensure that the expected map still matches that of the {receiver}.
      auto* receiver_map =
          AddNewNode<LoadTaggedField>({receiver}, HeapObject::kMapOffset);
      AddNewNode<CheckDynamicValue>({receiver_map, cache_type});
      auto* key = AddNewNode<LoadFixedArrayElement>({cache_array, index});
      SetAccumulator(key);

      current_for_in_state.receiver = receiver;
      if (ToObject* to_object =
              current_for_in_state.receiver->TryCast<ToObject>()) {
        current_for_in_state.receiver = to_object->value_input().node();
      }
      current_for_in_state.receiver_needs_map_check = false;
      current_for_in_state.cache_type = cache_type;
      current_for_in_state.key = key;
      if (hint == ForInHint::kEnumCacheKeysAndIndices) {
        current_for_in_state.index = index;
      }
      // We know that the enum cache entry is not undefined, so skip over the
      // next JumpIfUndefined.
      DCHECK(iterator_.next_bytecode() ==
                 interpreter::Bytecode::kJumpIfUndefined ||
             iterator_.next_bytecode() ==
                 interpreter::Bytecode::kJumpIfUndefinedConstant);
      iterator_.Advance();
      MergeDeadIntoFrameState(iterator_.GetJumpTargetOffset());
      break;
    }
    case ForInHint::kAny: {
      ValueNode* index = LoadRegisterTagged(1);
      ValueNode* context = GetContext();
      SetAccumulator(AddNewNode<ForInNext>(
          {context, receiver, cache_array, cache_type, index},
          feedback_source));
      break;
    };
  }
}

void MaglevGraphBuilder::VisitForInStep() {
  ValueNode* index = LoadRegisterInt32(0);
  SetAccumulator(AddNewNode<Int32NodeFor<Operation::kIncrement>>({index}));
  if (!in_peeled_iteration_) {
    // With loop peeling, only the `ForInStep` in the non-peeled loop body marks
    // the end of for-in.
    current_for_in_state = ForInState();
  }
}

void MaglevGraphBuilder::VisitSetPendingMessage() {
  ValueNode* message = GetAccumulatorTagged();
  SetAccumulator(AddNewNode<SetPendingMessage>({message}));
}

void MaglevGraphBuilder::VisitThrow() {
  ValueNode* exception = GetAccumulatorTagged();
  BuildCallRuntime(Runtime::kThrow, {exception});
  BuildAbort(AbortReason::kUnexpectedReturnFromThrow);
}
void MaglevGraphBuilder::VisitReThrow() {
  ValueNode* exception = GetAccumulatorTagged();
  BuildCallRuntime(Runtime::kReThrow, {exception});
  BuildAbort(AbortReason::kUnexpectedReturnFromThrow);
}

void MaglevGraphBuilder::VisitReturn() {
  // See also: InterpreterAssembler::UpdateInterruptBudgetOnReturn.
  const uint32_t relative_jump_bytecode_offset = iterator_.current_offset();
  if (ShouldEmitInterruptBudgetChecks() && relative_jump_bytecode_offset > 0) {
    AddNewNode<ReduceInterruptBudgetForReturn>({},
                                               relative_jump_bytecode_offset);
  }

  if (!is_inline()) {
    // We do not record a Tagged use on Return, since they are never on the hot
    // path, and will lead to a maximum of one additional Tagging operation in
    // the worst case. This allows loop accumulator to be untagged even if they
    // are later returned.
    FinishBlock<Return>(
        {GetAccumulatorTagged(UseReprHintRecording::kDoNotRecord)});
    return;
  }

  // All inlined function returns instead jump to one past the end of the
  // bytecode, where we'll later create a final basic block which resumes
  // execution of the caller. If there is only one return, at the end of the
  // function, we can elide this jump and just continue in the same basic block.
  if (iterator_.next_offset() != inline_exit_offset() ||
      NumPredecessors(inline_exit_offset()) > 1) {
    BasicBlock* block =
        FinishBlock<Jump>({}, &jump_targets_[inline_exit_offset()]);
    // The context is dead by now, set it to optimized out to avoid creating
    // unnecessary phis.
    SetContext(GetRootConstant(RootIndex::kOptimizedOut));
    MergeIntoInlinedReturnFrameState(block);
  }
}

void MaglevGraphBuilder::VisitThrowReferenceErrorIfHole() {
  // ThrowReferenceErrorIfHole <variable_name>
  compiler::NameRef name = GetRefOperand<Name>(0);
  ValueNode* value = GetRawAccumulator();

  // Avoid the check if we know it is not the hole.
  if (IsConstantNode(value->opcode())) {
    if (IsTheHoleValue(value)) {
      ValueNode* constant = GetConstant(name);
      BuildCallRuntime(Runtime::kThrowAccessedUninitializedVariable,
                       {constant});
      BuildAbort(AbortReason::kUnexpectedReturnFromThrow);
    }
    return;
  }

  // Avoid the check if {value}'s representation doesn't allow the hole.
  switch (value->value_representation()) {
    case ValueRepresentation::kInt32:
    case ValueRepresentation::kUint32:
    case ValueRepresentation::kFloat64:
    case ValueRepresentation::kHoleyFloat64:
      // Can't be the hole.
      // Note that HoleyFloat64 when converted to Tagged becomes Undefined
      // rather than the_hole, hence the early return for HoleyFloat64.
      return;

    case ValueRepresentation::kTagged:
      // Could be the hole.
      break;

    case ValueRepresentation::kWord64:
      UNREACHABLE();
  }

  // Avoid the check if {value} has an alternative whose representation doesn't
  // allow the hole.
  if (const NodeInfo* info = known_node_aspects().TryGetInfoFor(value)) {
    auto& alt = info->alternative();
    if (alt.int32() || alt.truncated_int32_to_number() || alt.float64()) {
      return;
    }
  }

  DCHECK(value->value_representation() == ValueRepresentation::kTagged);
  AddNewNode<ThrowReferenceErrorIfHole>({value}, name);
}

void MaglevGraphBuilder::VisitThrowSuperNotCalledIfHole() {
  // ThrowSuperNotCalledIfHole
  ValueNode* value = GetAccumulatorTagged();
  if (CheckType(value, NodeType::kJSReceiver)) return;
  // Avoid the check if we know it is not the hole.
  if (IsConstantNode(value->opcode())) {
    if (IsTheHoleValue(value)) {
      BuildCallRuntime(Runtime::kThrowSuperNotCalled, {});
      BuildAbort(AbortReason::kUnexpectedReturnFromThrow);
    }
    return;
  }
  AddNewNode<ThrowSuperNotCalledIfHole>({value});
}
void MaglevGraphBuilder::VisitThrowSuperAlreadyCalledIfNotHole() {
  // ThrowSuperAlreadyCalledIfNotHole
  ValueNode* value = GetAccumulatorTagged();
  // Avoid the check if we know it is the hole.
  if (IsConstantNode(value->opcode())) {
    if (!IsTheHoleValue(value)) {
      BuildCallRuntime(Runtime::kThrowSuperAlreadyCalledError, {});
      BuildAbort(AbortReason::kUnexpectedReturnFromThrow);
    }
    return;
  }
  AddNewNode<ThrowSuperAlreadyCalledIfNotHole>({value});
}
void MaglevGraphBuilder::VisitThrowIfNotSuperConstructor() {
  // ThrowIfNotSuperConstructor <constructor>
  ValueNode* constructor = LoadRegisterTagged(0);
  ValueNode* function = GetClosure();
  AddNewNode<ThrowIfNotSuperConstructor>({constructor, function});
}

void MaglevGraphBuilder::VisitSwitchOnGeneratorState() {
  // SwitchOnGeneratorState <generator> <table_start> <table_length>
  // It should be the first bytecode in the bytecode array.
  DCHECK_EQ(iterator_.current_offset(), 0);
  int generator_prologue_block_offset = 1;
  DCHECK_LT(generator_prologue_block_offset, next_offset());

  interpreter::JumpTableTargetOffsets offsets =
      iterator_.GetJumpTableTargetOffsets();
  // If there are no jump offsets, then this generator is not resumable, which
  // means we can skip checking for it and switching on its state.
  if (offsets.size() == 0) return;

  // We create an initial block that checks if the generator is undefined.
  ValueNode* maybe_generator = LoadRegisterTagged(0);
  // Neither the true nor the false path jump over any bytecode
  BasicBlock* block_is_generator_undefined = FinishBlock<BranchIfRootConstant>(
      {maybe_generator}, RootIndex::kUndefinedValue,
      &jump_targets_[next_offset()],
      &jump_targets_[generator_prologue_block_offset]);
  MergeIntoFrameState(block_is_generator_undefined, next_offset());

  // We create the generator prologue block.
  StartNewBlock(generator_prologue_block_offset, block_is_generator_undefined);

  // Generator prologue.
  ValueNode* generator = maybe_generator;
  ValueNode* state = AddNewNode<LoadTaggedField>(
      {generator}, JSGeneratorObject::kContinuationOffset);
  ValueNode* new_state = GetSmiConstant(JSGeneratorObject::kGeneratorExecuting);
  BuildStoreTaggedFieldNoWriteBarrier(generator, new_state,
                                      JSGeneratorObject::kContinuationOffset);
  ValueNode* context = AddNewNode<LoadTaggedField>(
      {generator}, JSGeneratorObject::kContextOffset);
  SetContext(context);

  // Guarantee that we have something in the accumulator.
  MoveNodeBetweenRegisters(iterator_.GetRegisterOperand(0),
                           interpreter::Register::virtual_accumulator());

  // Switch on generator state.
  int case_value_base = (*offsets.begin()).case_value;
  BasicBlockRef* targets = zone()->AllocateArray<BasicBlockRef>(offsets.size());
  for (interpreter::JumpTableTargetOffset offset : offsets) {
    BasicBlockRef* ref = &targets[offset.case_value - case_value_base];
    new (ref) BasicBlockRef(&jump_targets_[offset.target_offset]);
  }
  ValueNode* case_value = AddNewNode<UnsafeSmiUntag>({state});
  BasicBlock* generator_prologue_block = FinishBlock<Switch>(
      {case_value}, case_value_base, targets, offsets.size());
  for (interpreter::JumpTableTargetOffset offset : offsets) {
    MergeIntoFrameState(generator_prologue_block, offset.target_offset);
  }
}

void MaglevGraphBuilder::VisitSuspendGenerator() {
  // SuspendGenerator <generator> <first input register> <register count>
  // <suspend_id>
  ValueNode* generator = LoadRegisterTagged(0);
  ValueNode* context = GetContext();
  interpreter::RegisterList args = iterator_.GetRegisterListOperand(1);
  uint32_t suspend_id = iterator_.GetUnsignedImmediateOperand(3);

  int input_count = parameter_count_without_receiver() + args.register_count() +
                    GeneratorStore::kFixedInputCount;
  int debug_pos_offset = iterator_.current_offset() +
                         (BytecodeArray::kHeaderSize - kHeapObjectTag);
  AddNewNode<GeneratorStore>(
      input_count,
      [&](GeneratorStore* node) {
        int arg_index = 0;
        for (int i = 1 /* skip receiver */; i < parameter_count(); ++i) {
          node->set_parameters_and_registers(arg_index++, GetTaggedArgument(i));
        }
        const compiler::BytecodeLivenessState* liveness = GetOutLiveness();
        for (int i = 0; i < args.register_count(); ++i) {
          ValueNode* value = liveness->RegisterIsLive(args[i].index())
                                 ? GetTaggedValue(args[i])
                                 : GetRootConstant(RootIndex::kOptimizedOut);
          node->set_parameters_and_registers(arg_index++, value);
        }
      },

      context, generator, suspend_id, debug_pos_offset);

  FinishBlock<Return>({GetAccumulatorTagged()});
}

void MaglevGraphBuilder::VisitResumeGenerator() {
  // ResumeGenerator <generator> <first output register> <register count>
  ValueNode* generator = LoadRegisterTagged(0);
  ValueNode* array = AddNewNode<LoadTaggedField>(
      {generator}, JSGeneratorObject::kParametersAndRegistersOffset);
  interpreter::RegisterList registers = iterator_.GetRegisterListOperand(1);

  if (v8_flags.maglev_assert) {
    // Check if register count is invalid, that is, larger than the
    // register file length.
    ValueNode* array_length_smi =
        AddNewNode<LoadTaggedField>({array}, FixedArrayBase::kLengthOffset);
    ValueNode* array_length = AddNewNode<UnsafeSmiUntag>({array_length_smi});
    ValueNode* register_size = GetInt32Constant(
        parameter_count_without_receiver() + registers.register_count());
    AddNewNode<AssertInt32>(
        {register_size, array_length}, AssertCondition::kLessThanEqual,
        AbortReason::kInvalidParametersAndRegistersInGenerator);
  }

  const compiler::BytecodeLivenessState* liveness =
      GetOutLivenessFor(next_offset());
  RootConstant* stale = GetRootConstant(RootIndex::kStaleRegister);
  for (int i = 0; i < registers.register_count(); ++i) {
    if (liveness->RegisterIsLive(registers[i].index())) {
      int array_index = parameter_count_without_receiver() + i;
      StoreRegister(registers[i], AddNewNode<GeneratorRestoreRegister>(
                                      {array, stale}, array_index));
    }
  }
  SetAccumulator(AddNewNode<LoadTaggedField>(
      {generator}, JSGeneratorObject::kInputOrDebugPosOffset));
}

void MaglevGraphBuilder::VisitGetIterator() {
  // GetIterator <object>
  ValueNode* receiver = LoadRegisterTagged(0);
  ValueNode* context = GetContext();
  int load_slot = iterator_.GetIndexOperand(1);
  int call_slot = iterator_.GetIndexOperand(2);
  SetAccumulator(AddNewNode<GetIterator>({context, receiver}, load_slot,
                                         call_slot, feedback()));
}

void MaglevGraphBuilder::VisitDebugger() {
  BuildCallRuntime(Runtime::kHandleDebuggerStatement, {});
}

void MaglevGraphBuilder::VisitIncBlockCounter() {
  ValueNode* closure = GetClosure();
  ValueNode* coverage_array_slot = GetSmiConstant(iterator_.GetIndexOperand(0));
  BuildCallBuiltin<Builtin::kIncBlockCounter>({closure, coverage_array_slot});
}

void MaglevGraphBuilder::VisitAbort() {
  AbortReason reason = static_cast<AbortReason>(GetFlag8Operand(0));
  BuildAbort(reason);
}

void MaglevGraphBuilder::VisitWide() { UNREACHABLE(); }
void MaglevGraphBuilder::VisitExtraWide() { UNREACHABLE(); }
#define DEBUG_BREAK(Name, ...) \
  void MaglevGraphBuilder::Visit##Name() { UNREACHABLE(); }
DEBUG_BREAK_BYTECODE_LIST(DEBUG_BREAK)
#undef DEBUG_BREAK
void MaglevGraphBuilder::VisitIllegal() { UNREACHABLE(); }

}  // namespace v8::internal::maglev
