#include "torch/csrc/python_headers.h"

#include "torch/csrc/jit/python_tracer.h"
#include "torch/csrc/jit/tracer.h"
#include "torch/csrc/jit/export.h"
#include "torch/csrc/jit/pybind.h"
#include "torch/csrc/utils/python_strings.h"
#include "torch/csrc/jit/passes/dead_code_elimination.h"

#include "ATen/core/Error.h"

#include <sstream>

using namespace torch::autograd;
using namespace torch::jit;
using namespace torch::jit::tracer;


namespace torch { namespace jit { namespace tracer {


// Python interpreter retrieval routine adapted from
// https://stackoverflow.com/a/8706144
std::string getPythonInterpreterStackTrace() {
  std::stringstream stack_trace;
  AutoGIL gil;
  PyThreadState *tstate = PyThreadState_GET();
  if (NULL != tstate && NULL != tstate->frame) {
    PyFrameObject *frame = tstate->frame;

    while (NULL != frame) {
      int line = PyCode_Addr2Line(frame->f_code, frame->f_lasti);
      std::string filename = THPUtils_unpackString(frame->f_code->co_filename);
      std::string funcname = THPUtils_unpackString(frame->f_code->co_name);
      stack_trace << filename << "(" << line << "): " << funcname << "\n";
      frame = frame->f_back;
    }
  }
  return stack_trace.str();
}

std::shared_ptr<torch::jit::Graph> createGraphByTracing(
        py::function func,
        Stack trace_inputs,
        at::optional<size_t> num_real_inputs) {
  size_t num_func_inputs = num_real_inputs.value_or(trace_inputs.size());
  auto enter_info = tracer::enter(std::move(trace_inputs));
  try {

    py::tuple py_inputs(num_func_inputs);
    for(size_t i = 0; i < num_func_inputs; ++i) {
      py_inputs[i] = py::cast(enter_info.second[i]);
    }
    auto out = func(*py_inputs);
    if(!PyTuple_Check(out.ptr())) {
      out = py::make_tuple(out);
    }
    tracer::exit(toStack(out));
    auto graph = enter_info.first->graph;
    EliminateDeadCode(graph);
    return graph;
  } catch (...) {
    tracer::abandon();
    throw;
  }
}

Node* preRecordPythonTrace(THPObjectPtr pyobj,
                                  std::string arg_types,
                                  at::ArrayRef<Variable> inputs,
                                  pyobj_list scalar_args) {
  THPObjectPtr apply(PyObject_GetAttrString(pyobj.get(), "apply"));
  if(!apply) {
    throw python_error();
  }

  auto & graph = getTracingState()->graph;

  Node* n = graph->createPythonOp(
      std::move(apply), arg_types, std::move(scalar_args));
  recordSourceLocation(n);

  for (const Variable & input : inputs) {
    n->addInput(getValueTrace(input));
  }

  // NB: Order matters. This must append after inputs but before outputs.
  graph->appendNode(n);

  return n;
}

void pythonRecordSourceLocation(Node* n) {
  auto sl = std::make_shared<StringSourceLocation>(getPythonInterpreterStackTrace());
  n->setSourceLocation(sl);
}

void initPythonTracerBindings(PyObject* module) {
  setRecordSourceLocation(pythonRecordSourceLocation);

  auto m = py::handle(module).cast<py::module>();
  py::class_<TracingState,std::shared_ptr<TracingState>>(m, "TracingState", py::dynamic_attr())
    // NB: no constructor; you have to get it from C++ code
    .def("__repr__", [](const TracingState& s) {
      std::ostringstream ss;
      ss << "<TracingState " << (const void*)&s << ">";
      return ss.str();
    })
    .def("__str__", [](const TracingState& s) -> std::string {
      std::ostringstream ss;
      ss << *s.graph;
      return ss.str();
    })
    .def("push_scope", [](TracingState& s, const std::string& scope_name) {
      s.graph->push_scope(scope_name);
    })
    .def("pop_scope", [](TracingState& s) {
      s.graph->pop_scope();
    })
    .def("set_graph", [](TracingState& s, std::shared_ptr<Graph> g) {
      s.graph = g;
    })
    .def("graph", [](TracingState& s) {
      return s.graph;
    });

  m.def("_tracer_enter", [](py::args trace_inputs) {
    return tracer::enter(toStack(trace_inputs));
  });
  m.def("_tracer_exit", [](py::tuple var_outputs) {
    tracer::exit(toStack(var_outputs));
  });
  m.def("_tracer_abandon", []() {
    tracer::abandon();
  });
  m.def("_get_tracing_state", []() {
    return getTracingState();
  });
  m.def("_get_value_trace", [](const Variable& var) {
    return getValueTrace(var);
  });
  m.def("_set_value_trace", [](const Variable& var, Value* value) {
    return setValueTrace(var, value);
  });
}

}}} // namespace torch::jit::tracing
