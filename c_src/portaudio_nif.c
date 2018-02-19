#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <portaudio.h>

#include "erl_nif.h"

/**
 * Mark a parameter as unused.
 */
#define UNUSED(x) (void)(x)

/**
 * Make an erlang keyword list item. Represented as `{KEY, VAL}`.
 */
#define MAKE_KW_ITEM(ENV, KEY, VAL)                             \
  enif_make_tuple2((ENV), enif_make_atom((ENV), (KEY)), (VAL))

static ERL_NIF_TERM pa_error_to_error_tuple(ErlNifEnv *, PaError);

/**
 * Check a PortAudio return value for errors and return from
 * the current function with the related erlang term if found.
 */
#define HANDLE_PA_ERROR(ENV, ERR_OR_IDX)                    \
  {                                                         \
    if((ERR_OR_IDX) < 0) {                                  \
      return pa_error_to_error_tuple((ENV), (ERR_OR_IDX));  \
    }                                                       \
  }

struct err_to_str {
  PaError num;
  const char *str;
};

/**
 * Conversions from PortAudio errors to strings that can be easily converted
 * to erlang atoms.
 */
static struct err_to_str pa_errors[] = {
  { paNotInitialized,                        "not_initialized" },
  { paUnanticipatedHostError,                "unanticipated_host_error" },
  { paInvalidChannelCount,                   "invalid_channel_count" },
  { paInvalidSampleRate,                     "invalid_sample_rate" },
  { paInvalidDevice,                         "invalid_device" },
  { paInvalidFlag,                           "invalid_flag" },
  { paSampleFormatNotSupported,              "sample_format_unsupported" },
  { paBadIODeviceCombination,                "bad_device_combo" },
  { paInsufficientMemory,                    "insufficient_memory" },
  { paBufferTooBig,                          "buffer_too_big" },
  { paBufferTooSmall,                        "buffer_too_small" },
  { paNullCallback,                          "no_callback" },
  { paBadStreamPtr,                          "bad_callback" },
  { paTimedOut,                              "timeout" },
  { paInternalError,                         "internal_error" },
  { paDeviceUnavailable,                     "device_unavailable" },
  { paIncompatibleHostApiSpecificStreamInfo, "incompatible_host_stream_info" },
  { paStreamIsStopped,                       "stream_stopped" },
  { paStreamIsNotStopped,                    "stream_not_stopped" },
  { paInputOverflowed,                       "input_overflowed" },
  { paOutputUnderflowed,                     "output_underflowed" },
  { paHostApiNotFound,                       "no_host_api" },
  { paInvalidHostApi,                        "invalid_host_api" },
  { paCanNotReadFromACallbackStream,         "no_read_callback" },
  { paCanNotWriteToACallbackStream,          "no_write_callback" },
  { paCanNotReadFromAnOutputOnlyStream,      "output_only_stream" },
  { paCanNotWriteToAnInputOnlyStream,        "input_only_stream" },
  { paIncompatibleStreamHostApi,             "incompatible_host_api" },
  { paBadBufferPtr,                          "bad_buffer" },
  { 0, NULL } // SENTINEL
};

struct api_type_to_str {
  PaHostApiTypeId num;
  const char *str;
};

static struct api_type_to_str pa_drivers[] = {
  { paInDevelopment,   "in_development" },
  { paDirectSound,     "direct_sound" },
  { paMME,             "mme" },
  { paASIO,            "asio" },
  { paSoundManager,    "sound_manager" },
  { paCoreAudio,       "core_audio" },
  { paOSS,             "oss" },
  { paALSA,            "alsa" },
  { paAL,              "al" },
  { paBeOS,            "beos" },
  { paWDMKS,           "wdmks" },
  { paJACK,            "jack" },
  { paWASAPI,          "wasapi" },
  { paAudioScienceHPI, "audio_science_hpi" },
  { 0, NULL } // SENTINEL
};

struct portaudio_stream_handle
{
  // Thread management related
  bool      started;
  bool      should_stop;
  ErlNifTid thread;

  // Stream related
  PaStream *pa;
  PaStreamParameters *input;
  PaStreamParameters *output;
  int flags;
  double sample_rate;
  unsigned long frames_per_buffer;

  // Erlang related
  ErlNifPid *owner_pid;
  ErlNifPid *reader_pid;

  // Buffers and constants
  unsigned short input_frame_size;
  unsigned short input_sample_size;

  unsigned short output_frame_size;
  unsigned short output_sample_size;

  uint8_t total_samples;
};

static ERL_NIF_TERM str_to_binary_term(ErlNifEnv *env, const char *str)
{
  ERL_NIF_TERM binary;
  char *str_raw = (char *) enif_make_new_binary(env, strlen(str), &binary);
  assert(str_raw!= NULL);
  strcpy(str_raw, str);
  return binary;
}

/**
 * Return an error message for the given error code.
 */
const char *pa_error_to_char(PaError err)
{
  struct err_to_str *cur = &pa_errors[0];
  assert(cur != NULL); // should never happen

  while(cur->str != NULL) {
    if(cur->num == err) {
      return cur->str;
    }
    cur++;
  }

  return "unknown_error";
}

/**
 * Turn a PortAudio call into an error if there is one.
 */
static ERL_NIF_TERM pa_error_to_error_tuple(ErlNifEnv *env, PaError err)
{
  return enif_make_tuple2(env,
                          enif_make_atom(env, "error"),
                          enif_make_atom(env, pa_error_to_char(err)));
}

static ERL_NIF_TERM portaudio_version_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  const char *version = Pa_GetVersionText();
  return enif_make_tuple2(env,
                          enif_make_int(env, Pa_GetVersion()),
                          str_to_binary_term(env, version));
}

static ERL_NIF_TERM portaudio_default_host_api_index_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  return enif_make_int(env, Pa_GetDefaultHostApi());
}

static ERL_NIF_TERM portaudio_device_index_from_host_api_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  int host_api_index;
  int host_api_device_index;
  if(argc != 2
     || enif_get_int(env, argv[0], &host_api_index)
     || enif_get_int(env, argv[1], &host_api_device_index)) {
    return enif_make_badarg(env);
  }

  int device_index = Pa_HostApiDeviceIndexToDeviceIndex(host_api_index,
                                                        host_api_device_index);
  HANDLE_PA_ERROR(env, device_index);
  if(device_index < 0) {
    return pa_error_to_error_tuple(env, device_index);
  }
  return enif_make_int(env, device_index);
}

static ERL_NIF_TERM portaudio_host_api_info_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  int index;
  if(argc != 1
     || !enif_get_int(env, argv[0], &index)
     || index < 0
     || index >= Pa_GetHostApiCount()) {
    return enif_make_badarg(env);
  }

  const PaHostApiInfo *info = Pa_GetHostApiInfo(index);
  assert(info != NULL);

#define N_FIELDS 5
  const ERL_NIF_TERM fields[N_FIELDS] = {
    MAKE_KW_ITEM(env, "type",
                 enif_make_int(env, info->type)),
    MAKE_KW_ITEM(env, "name",
                 str_to_binary_term(env, info->name)),
    MAKE_KW_ITEM(env, "device_count",
                 enif_make_int(env, info->deviceCount)),
    MAKE_KW_ITEM(env, "default_input_device",
                 enif_make_int(env, info->defaultInputDevice)),
    MAKE_KW_ITEM(env, "default_output_device",
                 enif_make_int(env, info->defaultOutputDevice))
  };

  return enif_make_list_from_array(env, fields, N_FIELDS);
#undef N_FIELDS
}

static ERL_NIF_TERM portaudio_host_api_index_from_type_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  int type = -1;
  if(argc != 1) {
    return enif_make_badarg(env);
  }

  if(enif_is_atom(env, argv[0])) {
    struct api_type_to_str *cur = &pa_drivers[0];
    assert(cur != NULL); // should never happen

    while(cur->str != NULL) {
      if(enif_compare(enif_make_atom(env, cur->str), argv[0]) == 0) {
        type = cur->num;
        break;
      }
      cur++;
    }
  } else if(!enif_get_int(env, argv[0], &type)) {
    type = -1;
  }

  if(type == -1) {
    return enif_make_badarg(env);
  }

  int host_api_index = Pa_HostApiTypeIdToHostApiIndex(type);
  HANDLE_PA_ERROR(env, host_api_index);
  return enif_make_int(env, host_api_index);
}

static ERL_NIF_TERM portaudio_host_api_count_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  return enif_make_int(env, Pa_GetHostApiCount());
}

static ERL_NIF_TERM portaudio_default_input_device_index_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  return enif_make_int(env, Pa_GetDefaultInputDevice());
}

static ERL_NIF_TERM portaudio_default_output_device_index_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  return enif_make_int(env, Pa_GetDefaultOutputDevice());
}

static ERL_NIF_TERM portaudio_device_count_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  UNUSED(argc); UNUSED(argv);

  int num_devices = Pa_GetDeviceCount();
  HANDLE_PA_ERROR(env, num_devices);
  return enif_make_int(env, num_devices);
}

static ERL_NIF_TERM pa_device_info_to_term(ErlNifEnv *env, const PaDeviceInfo *device_info)
{
#define N_FIELDS 9
  // TODO: use a record or something
  const ERL_NIF_TERM fields[N_FIELDS] = {
    MAKE_KW_ITEM(env, "name",
                 str_to_binary_term(env, device_info->name)),
    MAKE_KW_ITEM(env, "host_api",
                 enif_make_int(env, device_info->hostApi)),
    MAKE_KW_ITEM(env, "max_input_channels",
                 enif_make_int(env, device_info->maxInputChannels)),
    MAKE_KW_ITEM(env, "max_output_channels",
                 enif_make_int(env, device_info->maxOutputChannels)),
    MAKE_KW_ITEM(env,
                 "default_low_input_latency",
                 enif_make_double(env, device_info->defaultLowInputLatency)),
    MAKE_KW_ITEM(env,
                 "default_low_output_latency",
                 enif_make_double(env, device_info->defaultLowOutputLatency)),
    MAKE_KW_ITEM(env,
                 "default_high_input_latency",
                 enif_make_double(env, device_info->defaultHighInputLatency)),
    MAKE_KW_ITEM(env,
                 "default_high_output_latency",
                 enif_make_double(env, device_info->defaultHighOutputLatency)),
    MAKE_KW_ITEM(env, "default_sample_rate",
                 enif_make_double(env, device_info->defaultSampleRate))
  };


  return enif_make_list_from_array(env, fields, N_FIELDS);
#undef N_FIELDS
}

static ERL_NIF_TERM portaudio_device_info_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
  int i;
  const PaDeviceInfo *device_info;

  if(argc != 1
     || !enif_get_int(env, argv[0], &i)
     || i >= Pa_GetDeviceCount()) {
    return enif_make_badarg(env);
  }

  device_info = Pa_GetDeviceInfo(i);
  assert(device_info != NULL);
  return pa_device_info_to_term(env, device_info);
}

static ErlNifFunc portaudio_nif_funcs[] =
  {
    {"version",      0, portaudio_version_nif, 0},
    // Native Host API
    {"default_host_api_index", 0, portaudio_default_host_api_index_nif, 0},
    {"device_index_from_host_api", 2, portaudio_device_index_from_host_api_nif, 0},
    {"host_api_count", 0, portaudio_host_api_count_nif, 0},
    {"host_api_info", 1, portaudio_host_api_info_nif, 0},
    {"host_api_index_from_type", 1, portaudio_host_api_index_from_type_nif, 0},
    // TODO: add support for checking types
    // Devices
    {"default_input_device_index", 0, portaudio_default_input_device_index_nif, 0},
    {"default_output_device_index", 0, portaudio_default_output_device_index_nif, 0},
    {"device_count", 0, portaudio_device_count_nif, 0},
    {"device_info",  1, portaudio_device_info_nif, 0},
  };

static int on_load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info)
{
  UNUSED(env); UNUSED(priv_data); UNUSED(load_info);

  PaError err = Pa_Initialize();

  if (err != paNoError) {
    return err;
  }

  return 0;
}

static void on_unload(ErlNifEnv *env, void *priv_data)
{
  UNUSED(env); UNUSED(priv_data);

  Pa_Terminate();
}

ERL_NIF_INIT(Elixir.PortAudio.Native,
             portaudio_nif_funcs,
             &on_load, NULL, NULL, &on_unload);
