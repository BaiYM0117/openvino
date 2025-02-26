//*****************************************************************************
// Copyright 2017-2021 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#pragma once

#include <pybind11/pybind11.h>

namespace py = pybind11;

void regclass_pyngraph_FrontEndManager(py::module m);
void regclass_pyngraph_FrontEnd(py::module m);
void regclass_pyngraph_InputModel(py::module m);
void regclass_pyngraph_FEC(py::module m);
void regclass_pyngraph_Place(py::module m);
