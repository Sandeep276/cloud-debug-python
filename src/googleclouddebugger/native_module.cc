/**
 * Copyright 2015 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Ensure that Python.h is included before any other header.
#include "common.h"

#include "breakpoints_emulator.h"
#include "bytecode_breakpoint.h"
#include "common.h"
#include "conditional_breakpoint.h"
#include "immutability_tracer.h"
#include "native_module.h"
#include "python_callback.h"
#include "python_util.h"
#include "rate_limit.h"

using google::LogMessage;

DEFINE_bool(
    enable_bytecode_rewrite_breakpoints,
    false,
    "Enables experimental support for zero overhead breakpoints instead of "
    "using profile/trace callbacks to emulate breakpoint support");

namespace devtools {
namespace cdbg {

const LogSeverity LOG_SEVERITY_INFO = ::google::INFO;
const LogSeverity LOG_SEVERITY_WARNING = ::google::WARNING;
const LogSeverity LOG_SEVERITY_ERROR = ::google::ERROR;

static const char kBreakpointsEmulatorKey[] = "breakpoints_emulator";

struct INTEGER_CONSTANT {
  const char* name;
  int32 value;
};

static const INTEGER_CONSTANT kIntegerConstants[] = {
  {
    "BREAKPOINT_EVENT_HIT",
    static_cast<int32>(BreakpointEvent::Hit)
  },
  {
    "BREAKPOINT_EVENT_EMULATOR_QUOTA_EXCEEDED",
    static_cast<int32>(BreakpointEvent::EmulatorQuotaExceeded)
  },
  {
    "BREAKPOINT_EVENT_ERROR",
    static_cast<int32>(BreakpointEvent::Error)
  },
  {
    "BREAKPOINT_EVENT_GLOBAL_CONDITION_QUOTA_EXCEEDED",
    static_cast<int32>(BreakpointEvent::GlobalConditionQuotaExceeded)
  },
  {
    "BREAKPOINT_EVENT_BREAKPOINT_CONDITION_QUOTA_EXCEEDED",
    static_cast<int32>(BreakpointEvent::BreakpointConditionQuotaExceeded)
  },
  {
    "BREAKPOINT_EVENT_CONDITION_EXPRESSION_MUTABLE",
    static_cast<int32>(BreakpointEvent::ConditionExpressionMutable)
  }
};

// Class to set zero overhead breakpoints.
// NOTE: not used as long as enable_bytecode_rewrite_breakpoints flag is false.
static BytecodeBreakpoint g_bytecode_breakpoint;

// Condition and dynamic logging rate limits are defined as the maximum
// amount of time in nanoseconds to spend on particular processing per
// second. These rate are enforced as following:
// 1. If a single breakpoint contributes to half the maximum rate, that
//    breakpoint will be deactivated.
// 2. If all breakpoints combined hit the maximum rate, any breakpoint to
//    exceed the limit gets disabled.
//
// The first rule ensures that in vast majority of scenarios expensive
// breakpoints will get deactivated. The second rule guarantees that in edge
// case scenarios the total amount of time spent in condition evaluation will
// not exceed the alotted limit.
//
// All limits ignore the number of CPUs since Python is inherently single
// threaded.
static std::unique_ptr<LeakyBucket> g_global_condition_quota_;

// Initializes C++ flags and logging.
//
// This function should be called exactly once during debugger bootstrap. It
// should be called before any other method in this module is used.
//
// If omitted, the module will stay with default C++ flag values and logging
// will go to stderr.
//
// Args:
//   flags: dictionary of all the flags (flags that don't match names of C++
//          flags will be ignored).
static PyObject* InitializeModule(PyObject* self, PyObject* py_args) {
  PyObject* flags = nullptr;
  if (!PyArg_ParseTuple(py_args, "O", &flags)) {
    return nullptr;
  }

  // Default to log to stderr unless explicitly overridden through flags.
  FLAGS_logtostderr = true;

  if (flags != Py_None) {
    if (!PyDict_Check(flags)) {
      PyErr_SetString(PyExc_TypeError, "flags must be None or a dictionary");
      return nullptr;
    }

    ScopedPyObject flag_items(PyDict_Items(flags));
    if (flag_items == nullptr) {
      PyErr_SetString(PyExc_TypeError, "Failed to iterate over items of flags");
      return nullptr;
    }

    int64 count = PyList_Size(flag_items.get());
    for (int64 i = 0; i < count; ++i) {
      PyObject* tuple = PyList_GetItem(flag_items.get(), i);
      if (tuple == nullptr) {  // Bad index (PyList_GetItem sets an exception).
        return nullptr;
      }

      const char* flag_name = nullptr;
      PyObject* flag_value_obj = nullptr;
      if (!PyArg_ParseTuple(tuple, "sO", &flag_name, &flag_value_obj)) {
        return nullptr;
      }

      ScopedPyObject flag_value_str_obj(PyObject_Str(flag_value_obj));
      if (flag_value_str_obj == nullptr) {
        PyErr_SetString(PyExc_TypeError, "Flag conversion to a string failed");
        return nullptr;
      }

      const char* flag_value = PyString_AsString(flag_value_str_obj.get());
      if (flag_value == nullptr) {  // Exception was already raised.
        return nullptr;
      }

      google::SetCommandLineOption(flag_name, flag_value);
    }
  }

  google::InitGoogleLogging("googleclouddebugger");

  Py_RETURN_NONE;
}


// Common code for LogXXX functions.
//
// The source file name and the source line are obtained automatically by
// inspecting the call stack.
//
// Args:
//   message: message to log.
//
// Returns: None
static PyObject* LogCommon(LogSeverity severity, PyObject* py_args) {
  const char* message = nullptr;
  if (!PyArg_ParseTuple(py_args, "s", &message)) {
    return nullptr;
  }

  const char* file_name = "<unknown>";
  int line = -1;

  PyFrameObject* frame = PyThreadState_Get()->frame;
  if (frame != nullptr) {
    file_name = PyString_AsString(frame->f_code->co_filename);
    line = PyFrame_GetLineNumber(frame);
  }

  // We only log file name, not the full path.
  if (file_name != nullptr) {
    const char* directory_end = strrchr(file_name, '/');
    if (directory_end != nullptr) {
      file_name = directory_end + 1;
    }
  }

  LogMessage(file_name, line, severity).stream() << message;

  Py_RETURN_NONE;
}


// Logs a message at INFO level from Python code.
static PyObject* LogInfo(PyObject* self, PyObject* py_args) {
  return LogCommon(LOG_SEVERITY_INFO, py_args);
}

// Logs a message at WARNING level from Python code.
static PyObject* LogWarning(PyObject* self, PyObject* py_args) {
  return LogCommon(LOG_SEVERITY_WARNING, py_args);
}


// Logs a message at ERROR level from Python code.
static PyObject* LogError(PyObject* self, PyObject* py_args) {
  return LogCommon(LOG_SEVERITY_ERROR, py_args);
}


// Searches for a statement with the specified line number in the specified
// code object.
//
// Args:
//   code_object: Python code object to analyze.
//   line: 1-based line number to search.
//
// Returns:
//   True if code_object includes a statement that maps to the specified
//   source line or False otherwise.
static PyObject* HasSourceLine(PyObject* self, PyObject* py_args) {
  PyCodeObject* code_object = nullptr;
  int line = -1;
  if (!PyArg_ParseTuple(py_args, "Oi", &code_object, &line)) {
    return nullptr;
  }

  if ((code_object == nullptr) || !PyCode_Check(code_object)) {
    PyErr_SetString(
        PyExc_TypeError,
        "code_object must be a code object");
    return nullptr;
  }

  CodeObjectLinesEnumerator enumerator(code_object);
  do {
    if (enumerator.line_number() == line) {
      Py_RETURN_TRUE;
    }
  } while (enumerator.Next());

  Py_RETURN_FALSE;
}


// Sets a new breakpoint in Python code. The breakpoint may have an optional
// condition to evaluate. When the breakpoint hits (and the condition matches)
// a callable object will be invoked from that thread.
//
// The breakpoint doesn't expire automatically after hit. It is the
// responsibility of the caller to call "ClearConditionalBreakpoint"
// appropriately.
//
// Args:
//   code_object: Python code object to set the breakpoint.
//   line: line number to set the breakpoint.
//   condition: optional callable object representing the condition to evaluate
//       or None for an unconditional breakpoint.
//   callback: callable object to invoke on breakpoint event. The callable is
//       invoked with two arguments: (event, frame). See "BreakpointFn" for more
//       details.
//
// Returns:
//   Integer cookie identifying this breakpoint. It needs to be specified when
//   clearing the breakpoint.
static PyObject* SetConditionalBreakpoint(PyObject* self, PyObject* py_args) {
  PyCodeObject* code_object = nullptr;
  int line = -1;
  PyCodeObject* condition = nullptr;
  PyObject* callback = nullptr;
  if (!PyArg_ParseTuple(py_args, "OiOO",
                        &code_object, &line, &condition, &callback)) {
    return nullptr;
  }

  if ((code_object == nullptr) || !PyCode_Check(code_object)) {
    PyErr_SetString(PyExc_TypeError, "invalid code_object argument");
    return nullptr;
  }

  if ((callback == nullptr) || !PyCallable_Check(callback)) {
    PyErr_SetString(PyExc_TypeError, "callback must be a callable object");
    return nullptr;
  }

  if (reinterpret_cast<PyObject*>(condition) == Py_None) {
    condition = nullptr;
  }

  if ((condition != nullptr) && !PyCode_Check(condition)) {
    PyErr_SetString(
        PyExc_TypeError,
        "condition must be None or a code object");
    return nullptr;
  }

  // Rate limiting has to be initialized before it is used for the first time.
  // We can't initialize it on module start because it happens before the
  // command line is parsed and flags are still at their default values.
  LazyInitializeRateLimit();

  auto conditional_breakpoint = std::make_shared<ConditionalBreakpoint>(
      ScopedPyCodeObject::NewReference(condition),
      ScopedPyObject::NewReference(callback));

  int cookie = -1;

  if (FLAGS_enable_bytecode_rewrite_breakpoints) {
    cookie = g_bytecode_breakpoint.SetBreakpoint(
        code_object,
        line,
        std::bind(
            &ConditionalBreakpoint::OnBreakpointEvent2<BreakpointEvent::Hit>,
            conditional_breakpoint),
        std::bind(
            &ConditionalBreakpoint::OnBreakpointEvent2<BreakpointEvent::Error>,
            conditional_breakpoint));
    if (cookie == -1) {
      conditional_breakpoint->OnBreakpointEvent(
          BreakpointEvent::Error,
          nullptr);
    }
  } else {
    auto* emulator = py_object_cast<BreakpointsEmulator>(
        GetDebugletModuleObject(kBreakpointsEmulatorKey));
    if (emulator == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "breakpoints emulator not found");
      return nullptr;
    }

    cookie = emulator->SetBreakpoint(
        code_object,
        line,
        std::bind(
            &ConditionalBreakpoint::OnBreakpointEvent,
            conditional_breakpoint,
            std::placeholders::_1,
            std::placeholders::_2));
  }

  return PyInt_FromLong(cookie);
}


// Clears the breakpoint previously set by "SetConditionalBreakpoint". Must be
// called exactly once per each call to "SetConditionalBreakpoint".
//
// Args:
//   cookie: breakpoint identifier returned by "SetConditionalBreakpoint".
static PyObject* ClearConditionalBreakpoint(PyObject* self, PyObject* py_args) {
  int cookie = -1;
  if (!PyArg_ParseTuple(py_args, "i", &cookie)) {
    return nullptr;
  }

  if (FLAGS_enable_bytecode_rewrite_breakpoints) {
    g_bytecode_breakpoint.ClearBreakpoint(cookie);
  } else {
    auto* emulator = py_object_cast<BreakpointsEmulator>(
        GetDebugletModuleObject(kBreakpointsEmulatorKey));
    if (emulator == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "breakpoints emulator not found");
      return nullptr;
    }

    emulator->ClearBreakpoint(cookie);
  }

  Py_RETURN_NONE;
}


// Disables breakpoints emulator for the current thread. No effect if zero
// overhead breakpoints are enabled.
// TODO(vlif): remove this function when breakpoint emulator is retired.
static PyObject* DisableDebuggerOnCurrentThread(
    PyObject* self,
    PyObject* py_args) {
  if (FLAGS_enable_bytecode_rewrite_breakpoints) {
    Py_RETURN_NONE;
  }

  return BreakpointsEmulator::DisableDebuggerOnCurrentThread(self, py_args);
}


// Invokes a Python callable object with immutability tracer.
//
// This ensures that the called method doesn't change any state, doesn't call
// unsafe native functions and doesn't take unreasonable amount of time to
// complete.
//
// This method supports multiple arguments to be specified. If no arguments
// needed, the caller should specify an empty tuple.
//
// Args:
//   frame: defines the evaluation context.
//   code: code object to invoke.
//
// Returns:
//   Return value of the callable.
static PyObject* CallImmutable(PyObject* self, PyObject* py_args) {
  PyObject* obj_frame = nullptr;
  PyObject* obj_code = nullptr;
  if (!PyArg_ParseTuple(py_args, "OO", &obj_frame, &obj_code)) {
    return nullptr;
  }

  if (!PyFrame_Check(obj_frame)) {
    PyErr_SetString(PyExc_TypeError, "argument 1 must be a frame object");
    return nullptr;
  }

  if (!PyCode_Check(obj_code)) {
    PyErr_SetString(PyExc_TypeError, "argument 2 must be a code object");
    return nullptr;
  }

  PyFrameObject* frame = reinterpret_cast<PyFrameObject*>(obj_frame);
  PyCodeObject* code = reinterpret_cast<PyCodeObject*>(obj_code);

  PyFrame_FastToLocals(frame);

  ScopedImmutabilityTracer immutability_tracer;
  return PyEval_EvalCode(code, frame->f_globals, frame->f_locals);
}


// Attaches the debuglet to the current thread.
//
// This is only needed for native threads as Python is not even aware they
// exist. If the debugger is already attached to this thread or if the
// debugger is disabled for this thread, this function does nothing.
void AttachNativeThread() {
  if (FLAGS_enable_bytecode_rewrite_breakpoints) {
    return;
  }

  auto* emulator = py_object_cast<BreakpointsEmulator>(
      GetDebugletModuleObject(kBreakpointsEmulatorKey));
  if (emulator == nullptr) {
    LOG(ERROR) << "Breakpoints emulator not found";
    return;
  }

  emulator->AttachNativeThread();
}

// Python wrapper of AttachNativeThread.
PyObject* PyAttachNativeThread(
    PyObject* self,
    PyObject* py_args) {
  AttachNativeThread();

  Py_INCREF(Py_None);
  return Py_None;
}




static PyMethodDef g_module_functions[] = {
  {
    "InitializeModule",
    InitializeModule,
    METH_VARARGS,
    "Initialize C++ flags and logging."
  },
  {
    "LogInfo",
    LogInfo,
    METH_VARARGS,
    "INFO level logging from Python code."
  },
  {
    "LogWarning",
    LogWarning,
    METH_VARARGS,
    "WARNING level logging from Python code."
  },
  {
    "LogError",
    LogError,
    METH_VARARGS,
    "ERROR level logging from Python code."
  },
  {
    "HasSourceLine",
    HasSourceLine,
    METH_VARARGS,
    "Checks whether Python code object includes the specified source "
    "line number."
  },
  {
    "SetConditionalBreakpoint",
    SetConditionalBreakpoint,
    METH_VARARGS,
    "Sets a new breakpoint in Python code."
  },
  {
    "ClearConditionalBreakpoint",
    ClearConditionalBreakpoint,
    METH_VARARGS,
    "Clears previously set breakpoint in Python code."
  },
  {
    "CallImmutable",
    CallImmutable,
    METH_VARARGS,
    "Invokes a Python callable object with immutability tracer."
  },
  {
    "AttachNativeThread",
    PyAttachNativeThread,
    METH_NOARGS,
    "Attaches the debugger to the current thread (only needed for native "
    "threads).",
  },
  {
    "DisableDebuggerOnCurrentThread",
    DisableDebuggerOnCurrentThread,
    METH_NOARGS,
    "Disables breakpoints emulator on the current thread "
    "(if not attached already).",
  },
  { nullptr, nullptr, 0, nullptr }  // sentinel
};


void InitDebuggerNativeModule() {
  PyObject* module = Py_InitModule3(
      CDBG_MODULE_NAME,
      g_module_functions,
      "Native module for Python Cloud Debugger");

  SetDebugletModule(module);

  if (!RegisterPythonType<PythonCallback>() ||
      !RegisterPythonType<ImmutabilityTracer>()) {
    return;
  }

  if (!FLAGS_enable_bytecode_rewrite_breakpoints) {
    if (!RegisterPythonType<ThreadBreakpoints>() ||
        !RegisterPythonType<BreakpointsEmulator>() ||
        !RegisterPythonType<DisableDebuggerKey>()) {
      return;
    }

    // Create singleton instance of "BreakpointsEmulator" and associate
    // it with the module.
    auto emulator = NewNativePythonObject<BreakpointsEmulator>();
    if (emulator == nullptr) {
      return;
    }

    if (PyModule_AddObject(module,
                           kBreakpointsEmulatorKey,
                           emulator.release())) {
      LOG(ERROR) << "Failed to add breakpoints emulator object to cdbg_native";
      return;
    }
  }

  // Add constants we want to share with the Python code.
  for (uint32 i = 0; i < arraysize(kIntegerConstants); ++i) {
    if (PyModule_AddObject(
          module,
          kIntegerConstants[i].name,
          PyInt_FromLong(kIntegerConstants[i].value))) {
      LOG(ERROR) << "Failed to constant " << kIntegerConstants[i].name
                 << " to native module";
      return;
    }
  }
}

}  // namespace cdbg
}  // namespace devtools



// This function is called to initialize the module.
PyMODINIT_FUNC initcdbg_native() {
  devtools::cdbg::InitDebuggerNativeModule();
}