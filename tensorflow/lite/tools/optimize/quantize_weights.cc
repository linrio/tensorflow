/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/tools/optimize/quantize_weights.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/flexbuffers.h"
#include "absl/memory/memory.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/lite/context.h"
#include "tensorflow/lite/kernels/internal/tensor_utils.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/tools/optimize/quantization_utils.h"

namespace tflite {
namespace optimize {

namespace {

typedef struct {
  OperatorT* op;
  // The index of the op in the operators vector.
  int32_t op_idx;
  // The index of the tensor to quantize in subgraph->tensors.
  int32_t op_input_idx;
} ConsumerOpInfo;

// The default minimum number of elements a weights array must have to be
// quantized by this transformation.
const int kWeightsMinNumElementsDefault = 1024;

// Gets the operators that consume tensor_idx.
std::vector<ConsumerOpInfo> GetTensorConsumers(const ModelT* model,
                                               const SubGraphT* subgraph,
                                               int32_t tensor_idx) {
  // TODO(suharshs): If this proves to be too slow, avoid calling it per tensor,
  // instead doing one sweep for the entire model.
  std::vector<ConsumerOpInfo> consumer_ops;
  for (size_t op_idx = 0; op_idx < subgraph->operators.size(); ++op_idx) {
    OperatorT* op = subgraph->operators[op_idx].get();
    if (op == nullptr) {
      continue;
    }
    for (size_t i = 0; i < op->inputs.size(); ++i) {
      if (op->inputs[i] == tensor_idx) {
        consumer_ops.push_back(
            {op, static_cast<int32_t>(op_idx), static_cast<int32_t>(i)});
      }
    }
  }
  return consumer_ops;
}

// Gets the list of op->inputs indices of the weights inputs to be quantized for
// the provided op.
std::vector<int32_t> GetWeightInputIndices(const BuiltinOperator& op_code) {
  if (op_code == BuiltinOperator_CONV_2D ||
      op_code == BuiltinOperator_DEPTHWISE_CONV_2D ||
      op_code == BuiltinOperator_FULLY_CONNECTED ||
      op_code == BuiltinOperator_EMBEDDING_LOOKUP) {
    return {1};
  } else if (op_code == BuiltinOperator_SVDF) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/svdf.cc
    return {1, 2};
  } else if (op_code == BuiltinOperator_LSTM ||
             op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_LSTM) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/lstm.cc
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/unidirectional_sequence_lstm.cc
    return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16};
  } else if (op_code == BuiltinOperator_RNN ||
             op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_RNN) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/basic_rnn.cc
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/unidirectional_sequence_rnn.cc
    return {1, 2};
  } else if (op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_LSTM) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/bidirectional_sequence_lstm.cc
    return {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 16,
            18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 33};
  } else if (op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_RNN) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/bidirectional_sequence_rnn.cc
    return {1, 2, 4, 5};
  } else if (op_code == BuiltinOperator_GATHER) {
    // https://www.tensorflow.org/code/tensorflow/lite/kernels/gather.cc
    return {0};
  }
  return {};
}

// Returns true if the operator supports hybrid evaluation.
bool IsHybridEvaluationOp(const OperatorT* op, const BuiltinOperator& op_code) {
  // Operations that support hybrid evaluation.
  bool eval_hybrid = false;
  if (op_code == BuiltinOperator_FULLY_CONNECTED ||
      op_code == BuiltinOperator_CONV_2D || op_code == BuiltinOperator_SVDF ||
      op_code == BuiltinOperator_EMBEDDING_LOOKUP ||
      op_code == BuiltinOperator_RNN ||
      op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_LSTM ||
      op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_RNN ||
      op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_LSTM ||
      op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_RNN) {
    eval_hybrid = true;
  } else if (op_code == BuiltinOperator_LSTM) {
    const LSTMOptionsT* options = op->builtin_options.AsLSTMOptions();
    // Only lstm kernel_type full supports hybrid evaluation.
    if (options->kernel_type == LSTMKernelType_FULL) {
      eval_hybrid = true;
    }
  }
  return eval_hybrid;
}

// Returns true if all of the op's inputs are quantized.
bool CheckAllOpInputsQuantized(const SubGraphT* subgraph, const OperatorT* op,
                               const BuiltinOperator& op_code) {
  std::vector<int32_t> op_input_indices = GetWeightInputIndices(op_code);
  for (const int32_t op_input_idx : op_input_indices) {
    int32_t tensor_idx = op->inputs[op_input_idx];

    if (tensor_idx == -1) {
      // Optional tensor.
      continue;
    }

    TensorT* tensor = subgraph->tensors[tensor_idx].get();

    if (tensor->type != TensorType_INT8) {
      return false;
    }
  }
  return true;
}

// Inserts Tensors for each input tensor of op that should be
// quantized into tensor_map.
TfLiteStatus InsertQuantizableInputTensorsFromOperator(
    const ModelT* model, const OperatorT* op, uint64_t weights_min_num_elements,
    std::unordered_map<int32_t, TensorT*>* tensor_map) {
  SubGraphT* subgraph = model->subgraphs.at(0).get();
  const BuiltinOperator op_code =
      model->operator_codes[op->opcode_index]->builtin_code;

  std::vector<int32_t> op_input_indices = GetWeightInputIndices(op_code);
  for (const int32_t op_input_idx : op_input_indices) {
    int32_t tensor_idx = op->inputs[op_input_idx];
    if (tensor_idx == -1) {
      LOG(INFO) << "Skipping optional tensor input " << op_input_idx
                << " of operation " << EnumNameBuiltinOperator(op_code);
      continue;
    }

    TensorT* tensor = subgraph->tensors[tensor_idx].get();
    if (tensor->type != TensorType_FLOAT32) {
      LOG(INFO) << "Skipping quantization of tensor " << tensor->name
                << " that is not type float.";
      continue;
    }

    uint64_t num_elements;
    TF_LITE_ENSURE_STATUS(utils::NumElements(*tensor, &num_elements));
    if (num_elements < weights_min_num_elements) {
      LOG(INFO) << "Skipping quantization of tensor " << tensor->name
                << " because it has fewer than " << weights_min_num_elements
                << " elements (" << num_elements << ").";
      continue;
    }

    // Some tensors may have a null buffer vector, indicating an intermediate
    // array.
    if (model->buffers[tensor->buffer]->data.data() == nullptr) {
      LOG(INFO) << "Skipping quantization of tensor " << tensor->name
                << " because it has no allocated buffer.";
      continue;
    }

    tensor_map->insert({tensor_idx, tensor});
  }

  return kTfLiteOk;
}

// Returns the index of the Dequantize op_code.
// If a Dequantize op_code doesn't exist, adds it and returns its index.
int32_t GetOrInsertDequantizeOpCodeIndex(ModelT* model) {
  for (int i = 0; i < model->operator_codes.size(); ++i) {
    if (model->operator_codes[i]->builtin_code == BuiltinOperator_DEQUANTIZE) {
      return i;
    }
  }
  model->operator_codes.push_back(absl::make_unique<OperatorCodeT>());
  int op_code_idx = model->operator_codes.size() - 1;
  model->operator_codes[op_code_idx]->builtin_code = BuiltinOperator_DEQUANTIZE;
  // Version 2 and onwards supports INT8 inputs.
  model->operator_codes[op_code_idx]->version = 2;

  // Return the index of the newly placed OperatorCodeT.
  return op_code_idx;
}

// Creates a Dequantize OperatorT object.
void MakeDequantizeOperator(ModelT* model, std::unique_ptr<OperatorT>* op,
                            int32_t input, int32_t output) {
  OperatorT* op_raw = new OperatorT;
  op_raw->opcode_index = GetOrInsertDequantizeOpCodeIndex(model);
  op_raw->inputs = {input};
  op_raw->outputs = {output};

  op->reset(op_raw);
}

// Create a new TensorT object.
void MakeTensor(const string& name, const std::vector<int32_t>& shape,
                std::unique_ptr<TensorT>* tensor) {
  TensorT* tensor_raw = new TensorT;
  tensor_raw->name = name;
  tensor_raw->shape = shape;

  tensor->reset(tensor_raw);
}

// Updates operator code versions for the operators with INT8 inputs.
void UpdateInt8OperatorVersions(ModelT* model) {
  for (int i = 0; i < model->operator_codes.size(); ++i) {
    const BuiltinOperator& op_code = model->operator_codes[i]->builtin_code;
    if (op_code == BuiltinOperator_CONV_2D || op_code == BuiltinOperator_SVDF ||
        op_code == BuiltinOperator_EMBEDDING_LOOKUP ||
        op_code == BuiltinOperator_RNN ||
        op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_RNN ||
        op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_LSTM ||
        op_code == BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_RNN) {
      model->operator_codes[i]->version = 2;

    } else if (op_code == BuiltinOperator_FULLY_CONNECTED ||
               op_code == BuiltinOperator_BIDIRECTIONAL_SEQUENCE_LSTM ||
               op_code == BuiltinOperator_LSTM) {
      model->operator_codes[i]->version = 3;
    }
  }
}

// Returns true if the op in consumer_op_infos can pass through quantization.
bool IsQuantizationPassThroughOps(
    const ModelT* model, const std::vector<ConsumerOpInfo>& consumer_op_infos) {
  if (consumer_op_infos.size() != 1) {
    return false;
  }
  const OperatorT* consumer_op = consumer_op_infos.front().op;
  const BuiltinOperator op_code =
      model->operator_codes[consumer_op->opcode_index]->builtin_code;
  return op_code == BuiltinOperator_GATHER;
}

// Copies quantization parameters from input to output and returns consumers of
// the output tensor as a tuple with values:
// - index of the output tensor
// - pointer to the output tensor
// - vector of consumers ops.
std::tuple<int32_t, TensorT*, std::vector<ConsumerOpInfo>>
PassQuantizationAndGetConsumers(
    const ModelT* model, const SubGraphT* subgraph,
    const std::vector<ConsumerOpInfo>& consumer_op_infos) {
  const OperatorT* op = consumer_op_infos.front().op;
  const BuiltinOperator op_code =
      model->operator_codes[op->opcode_index]->builtin_code;
  if (op->outputs.size() != 1) {
    LOG(ERROR)
        << "An op that passes quantization has more than one quantized output";
    return std::make_tuple(-1, nullptr, std::vector<ConsumerOpInfo>());
  }
  const int32_t output_tensor_idx = op->outputs.front();
  const auto input_idx = GetWeightInputIndices(op_code);
  if (input_idx.size() != 1) {
    LOG(ERROR)
        << "An op that passes quantization has more than one quantized input";
    return std::make_tuple(-1, nullptr, std::vector<ConsumerOpInfo>());
  }
  const int32_t input_tensor_idx = op->inputs[input_idx.front()];

  // Propagate quantization params.
  const TensorT* input_tensor = subgraph->tensors[input_tensor_idx].get();
  TensorT* output_tensor = subgraph->tensors[output_tensor_idx].get();
  if (!output_tensor->quantization) {
    output_tensor->quantization = absl::make_unique<QuantizationParametersT>();
  }
  *output_tensor->quantization = *input_tensor->quantization;
  output_tensor->type = TensorType_INT8;
  return std::make_tuple(
      output_tensor_idx, output_tensor,
      GetTensorConsumers(model, subgraph, output_tensor_idx));
}

TfLiteStatus QuantizeWeightsInternal(flatbuffers::FlatBufferBuilder* builder,
                                     const Model* input_model,
                                     bool use_hybrid_evaluation,
                                     uint64_t weights_min_num_elements) {
  std::unique_ptr<ModelT> model;
  model.reset(input_model->UnPack());

  // TODO(suharshs): When models support multiple subgraphs, add support.
  if (model->subgraphs.size() != 1) {
    LOG(ERROR) << "Quantize weights tool only supports tflite models with one "
                  "subgraph.";
    return kTfLiteError;
  }

  SubGraphT* subgraph = model->subgraphs.at(0).get();

  std::vector<std::unique_ptr<OperatorT>> new_operators;
  std::unordered_map<int32_t, TensorT*> tensor_map;
  for (int i = 0; i < subgraph->operators.size(); ++i) {
    OperatorT* op = subgraph->operators[i].get();
    TF_LITE_ENSURE_STATUS(InsertQuantizableInputTensorsFromOperator(
        model.get(), op, weights_min_num_elements, &tensor_map));
  }

  // The unordered_map ensures that we quantize each tensor exactly once.
  // TODO(suharshs): This map key isn't sufficient when we support multiple
  // subgraphs.
  for (std::pair<int32_t, TensorT*> tensor_pair : tensor_map) {
    // Quantize the tensor.
    TF_LITE_ENSURE_STATUS(
        utils::SymmetricQuantizeTensor(model.get(), tensor_pair.second));
  }

  // Examine the tensor consumers to determine which require dequantize ops.
  for (const auto& tensor_pair : tensor_map) {
    int32_t tensor_idx = tensor_pair.first;
    TensorT* tensor = tensor_pair.second;
    std::vector<ConsumerOpInfo> consumer_op_infos =
        GetTensorConsumers(model.get(), subgraph, tensor_idx);
    if (IsQuantizationPassThroughOps(model.get(), consumer_op_infos)) {
      std::tie(tensor_idx, tensor, consumer_op_infos) =
          PassQuantizationAndGetConsumers(model.get(), subgraph,
                                          consumer_op_infos);
      if (tensor_idx < 0) {
        // Error message is already logged by PassQuantizationAndGetConsumers.
        return kTfLiteError;
      }
    }

    std::vector<ConsumerOpInfo> dequant_op_infos;  // Ops that need dequants.
    for (ConsumerOpInfo& consumer_op_info : consumer_op_infos) {
      OperatorT* consumer_op = consumer_op_info.op;
      const BuiltinOperator consumer_op_code =
          model->operator_codes[consumer_op->opcode_index]->builtin_code;
      // If the op is a hybrid op and all the required tensors are quantized,
      // we have no further work to do, but for all ops that require
      // dequantization we need to add a Dequantize op.
      bool eval_hybrid =
          use_hybrid_evaluation &&
          IsHybridEvaluationOp(consumer_op, consumer_op_code) &&
          CheckAllOpInputsQuantized(subgraph, consumer_op, consumer_op_code);
      if (!eval_hybrid) {
        dequant_op_infos.push_back(consumer_op_info);
      }
    }

    // Check that this tensor is an output tensor.
    int32_t output_index = -1;
    for (int32_t i = 0; i < subgraph->outputs.size(); ++i) {
      if (subgraph->outputs[i] == tensor_idx) {
        output_index = i;
        break;
      }
    }

    // If no ops require dequant and it is not output, we are done for this
    // tensor.
    if (dequant_op_infos.empty() && output_index < 0) {
      continue;
    }

    // Create a new tensor to be the output of the dequantize op.
    std::unique_ptr<TensorT> dequantize_output;
    const string dequant_name = tensor->name + "_dequantize";
    MakeTensor(dequant_name, tensor->shape, &dequantize_output);
    const int32_t dequantize_output_idx = subgraph->tensors.size();
    subgraph->tensors.push_back(std::move(dequantize_output));

    // Create the Dequantize operation.
    std::unique_ptr<OperatorT> dequantize_op;
    MakeDequantizeOperator(model.get(), &dequantize_op, tensor_idx,
                           dequantize_output_idx);

    LOG(INFO) << "Creating Dequantize op with name " << dequant_name << ".";

    // Update the op_input of all the ops that need the created dequantize
    // operation.
    int32_t min_op_idx = subgraph->operators.size();
    for (ConsumerOpInfo& dequant_op_info : dequant_op_infos) {
      dequant_op_info.op->inputs[dequant_op_info.op_input_idx] =
          dequantize_output_idx;
      min_op_idx = std::min(dequant_op_info.op_idx, min_op_idx);
    }
    // Update output name.
    if (output_index >= 0) {
      subgraph->outputs[output_index] = dequantize_output_idx;
    }

    // Insert the newly created Dequantize operation before the earliest
    // consumer, since TFLite requires operators to be topo-sorted.
    subgraph->operators.insert(subgraph->operators.begin() + min_op_idx,
                               std::move(dequantize_op));
  }

  // Update the modified operator code versions.
  UpdateInt8OperatorVersions(model.get());

  flatbuffers::Offset<Model> output_model_location =
      Model::Pack(*builder, model.get());
  FinishModelBuffer(*builder, output_model_location);

  return kTfLiteOk;
}

}  // namespace

namespace internal {
TfLiteStatus QuantizeWeights(flatbuffers::FlatBufferBuilder* builder,
                             const Model* input_model,
                             uint64_t weights_min_num_elements,
                             bool use_hybrid_evaluation) {
  // By default we require that only weights with more than
  // kWeightsMinSizeDefault elements are quantized.
  return QuantizeWeightsInternal(builder, input_model, use_hybrid_evaluation,
                                 weights_min_num_elements);
}
}  // namespace internal

TfLiteStatus QuantizeWeights(flatbuffers::FlatBufferBuilder* builder,
                             const Model* input_model,
                             uint64_t weights_min_num_elements) {
  return QuantizeWeightsInternal(builder, input_model, true,
                                 weights_min_num_elements);
}

TfLiteStatus QuantizeWeights(flatbuffers::FlatBufferBuilder* builder,
                             const Model* input_model) {
  // By default we require that only weights with more than
  // kWeightsMinSizeDefault elements are quantized.
  return QuantizeWeightsInternal(builder, input_model, true,
                                 kWeightsMinNumElementsDefault);
}

}  // namespace optimize
}  // namespace tflite
