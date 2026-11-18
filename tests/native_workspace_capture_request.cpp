#include "slic3r/GUI/WorkspaceCaptureRequest.hpp"

#include <cassert>
#include <functional>

using nlohmann::json;
using Slic3r::GUI::validate_workspace_capture_request;

int main()
{
    const json overview = {{"format_version", 1}, {"width", 1600}, {"height", 1000},
        {"plates", json::array()}, {"objects", json::array()},
        {"visibility", {{"beds", true}, {"plate_identifiers", true}, {"prime_towers", true}}},
        {"camera", {{"preset", "isometric"}, {"projection", "orthographic"}, {"margin", 1.1}}}};
    validate_workspace_capture_request(overview);
    auto custom = overview;
    custom["plates"] = {1, 3};
    custom["objects"] = {2};
    custom["camera"]["orientation"] = {0.9238795, 0.3826834, 0.0, 0.0};
    custom["camera"]["target"] = {200.0, -150.0, 12.5};
    custom["camera"]["zoom"] = 2.5;
    custom["camera"]["projection"] = "perspective";
    validate_workspace_capture_request(json::parse(custom.dump()));

    const auto rejects = [&](const std::function<void(json&)>& mutate) {
        auto invalid = overview;
        mutate(invalid);
        bool rejected = false;
        try { validate_workspace_capture_request(invalid); }
        catch (const std::exception&) { rejected = true; }
        assert(rejected);
    };
    rejects([](json& value) { value["width"] = 0; });
    rejects([](json& value) { value["width"] = 1.5; });
    rejects([](json& value) { value["height"] = 4294967297ULL; });
    rejects([](json& value) { value["width"] = 8192; value["height"] = 8192; });
    rejects([](json& value) { value["plates"] = {1, 1}; });
    rejects([](json& value) { value["objects"] = {-1}; });
    rejects([](json& value) { value["visibility"]["beds"] = "false"; });
    rejects([](json& value) { value["camera"]["projection"] = "fisheye"; });
    rejects([](json& value) { value["camera"]["orientation"] = {0, 0, 0, 0}; });
    rejects([](json& value) { value["camera"]["orientation"] = {1, 0, 0}; });
    rejects([](json& value) { value["camera"]["target"] = {0, 0}; });
    rejects([](json& value) { value["camera"]["zoom"] = -1; });
    rejects([](json& value) { value["camera"]["margin"] = 0.5; });
}
