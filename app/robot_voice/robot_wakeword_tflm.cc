/* TFLite Micro bridge for the imported INT8 wakeword model. */
#include "robot_wakeword_config.h"
#include "robot_wakeword_model.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <new>
#include <string.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace
{
uint8_t *g_arena;
alignas(tflite::MicroInterpreter)
unsigned char g_interpreter_storage[sizeof(tflite::MicroInterpreter)];
tflite::MicroMutableOpResolver<3> g_resolver;
tflite::MicroInterpreter *g_interpreter;
TfLiteTensor *g_input;
TfLiteTensor *g_output;
bool g_operators_ready;

bool valid_tensor(const TfLiteTensor *tensor, const int *shape, int rank)
{
  if (!tensor || tensor->type != kTfLiteInt8 || !tensor->data.int8 ||
      !tensor->dims || tensor->dims->size != rank ||
      !isfinite(tensor->params.scale) || tensor->params.scale <= 0.0f ||
      tensor->params.zero_point < -128 || tensor->params.zero_point > 127)
    {
      return false;
    }

  for (int i = 0; i < rank; ++i)
    {
      if (tensor->dims->data[i] != shape[i])
        {
          return false;
        }
    }

  return true;
}

int invoke(float *score)
{
  if (g_interpreter->Invoke() != kTfLiteOk)
    {
      return -EIO;
    }

  const float positive =
    (static_cast<int>(g_output->data.int8[1]) -
     g_output->params.zero_point) * g_output->params.scale;
  *score = fminf(1.0f, fmaxf(0.0f, positive));
  return 0;
}
}  // namespace

extern "C" int robot_wakeword_model_init(void)
{
  if (g_interpreter)
    {
      return 0;
    }

  const robot_wakeword_model_config_s &config = g_robot_wakeword_model_config;
  if (g_robot_wakeword_model_size < 8 || !config.time_bins ||
      !config.mel_bins || config.time_bins > 160000 || config.mel_bins > 128)
    {
      return -EINVAL;
    }

  const tflite::Model *model = tflite::GetModel(g_robot_wakeword_model);
  if (!model || model->version() != TFLITE_SCHEMA_VERSION)
    {
      return -EINVAL;
    }

  if (!g_operators_ready)
    {
      if (g_resolver.AddReshape() != kTfLiteOk ||
          g_resolver.AddFullyConnected() != kTfLiteOk ||
          g_resolver.AddSoftmax() != kTfLiteOk)
        {
          return -EINVAL;
        }

      g_operators_ready = true;
    }

  g_arena = static_cast<uint8_t *>(memalign(16, ROBOT_WAKEWORD_ARENA_BYTES));
  if (!g_arena)
    {
      return -ENOMEM;
    }

  g_interpreter = new (g_interpreter_storage) tflite::MicroInterpreter(
    model, g_resolver, g_arena, ROBOT_WAKEWORD_ARENA_BYTES);
  if (g_interpreter->AllocateTensors() != kTfLiteOk)
    {
      robot_wakeword_model_close();
      return -ENOMEM;
    }

  const int input_shape[] = {1, static_cast<int>(config.time_bins),
                            static_cast<int>(config.mel_bins), 1};
  const int output_shape[] = {1, 2};
  if (g_interpreter->inputs_size() != 1 ||
      g_interpreter->outputs_size() != 1 ||
      !valid_tensor(g_interpreter->input(0), input_shape, 4) ||
      !valid_tensor(g_interpreter->output(0), output_shape, 2))
    {
      robot_wakeword_model_close();
      return -EINVAL;
    }

  g_input = g_interpreter->input(0);
  g_output = g_interpreter->output(0);
  return 0;
}

extern "C" void robot_wakeword_model_close(void)
{
  if (g_interpreter)
    {
      g_interpreter->~MicroInterpreter();
    }

  free(g_arena);

  g_interpreter = nullptr;
  g_arena = nullptr;
  g_input = nullptr;
  g_output = nullptr;
}

extern "C" int robot_wakeword_model_predict(const float *features, float *score)
{
  if (!features || !score)
    {
      return -EINVAL;
    }

  *score = 0.0f;
  if (!g_interpreter)
    {
      return -ENODEV;
    }

  const size_t count =
    static_cast<size_t>(g_robot_wakeword_model_config.time_bins) *
    g_robot_wakeword_model_config.mel_bins;
  for (size_t i = 0; i < count; ++i)
    {
      if (!isfinite(features[i]))
        {
          return -EINVAL;
        }

      const float quantized = features[i] / g_input->params.scale +
                              g_input->params.zero_point;
      g_input->data.int8[i] = quantized <= -128.0f ? -128 :
                             quantized >= 127.0f ? 127 :
                             static_cast<int8_t>(lrintf(quantized));
    }

  return invoke(score);
}

extern "C" int robot_wakeword_model_test(float *score)
{
  if (!score)
    {
      return -EINVAL;
    }

  *score = 0.0f;
  if (!g_interpreter)
    {
      return -ENODEV;
    }

  const size_t count =
    static_cast<size_t>(g_robot_wakeword_model_config.time_bins) *
    g_robot_wakeword_model_config.mel_bins;
  if (count != g_robot_wakeword_test_input_size)
    {
      return -EINVAL;
    }

  memcpy(g_input->data.int8, g_robot_wakeword_test_input, count);
  int ret = invoke(score);
  if (ret == 0 && memcmp(g_output->data.int8,
                         g_robot_wakeword_test_output, 2) != 0)
    {
      return -EIO;
    }

  return ret;
}
