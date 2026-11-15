#pragma once

#include <string>

namespace Slic3r { namespace GUI {

// The caller owns the disposable application, display, working directory and
// deadline. Both the source project and the request are read-only inputs.
void capture_workspace(const std::string& request_path);

}}
