#pragma once
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#define ROBOT_WAKEWORD_REQUIRED_OP_COUNT 5
using RobotWakewordOpResolver = tflite::MicroMutableOpResolver<ROBOT_WAKEWORD_REQUIRED_OP_COUNT>;
inline TfLiteStatus RobotWakewordRegisterOps(RobotWakewordOpResolver &resolver) {
  if (resolver.AddConv2D() != kTfLiteOk) return kTfLiteError;
  if (resolver.AddDepthwiseConv2D() != kTfLiteOk) return kTfLiteError;
  if (resolver.AddFullyConnected() != kTfLiteOk) return kTfLiteError;
  if (resolver.AddReshape() != kTfLiteOk) return kTfLiteError;
  if (resolver.AddSoftmax() != kTfLiteOk) return kTfLiteError;
  return kTfLiteOk;
}
