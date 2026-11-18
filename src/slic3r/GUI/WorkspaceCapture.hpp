#pragma once

#include <string>
#include <stdexcept>

namespace Slic3r { namespace GUI {

constexpr int workspace_capture_unavailable_exit_code = 3;
class WorkspaceCaptureUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The caller owns the disposable application, display, working directory and
// deadline and validates the request before importing the source project.
void capture_workspace(const std::string& request_json, const std::string& output_directory);

}}
