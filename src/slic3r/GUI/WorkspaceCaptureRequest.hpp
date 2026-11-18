#pragma once

#include "nlohmann/json.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace Slic3r { namespace GUI {

// This validation has no application or graphics side effects. The worker and
// command-line entry point can reject invalid requests before opening a project.
inline void validate_workspace_capture_request(const nlohmann::json& request)
{
    using nlohmann::json;
    const auto fail = [](const char* message) { throw std::runtime_error(message); };
    const auto positive_index = [&](const json& value) {
        return value.is_number_integer() && value.get<double>() > 0 &&
            value.get<double>() <= std::numeric_limits<int>::max();
    };
    if (!request.is_object() || request.at("format_version") != 1 ||
        !positive_index(request.at("width")) || !positive_index(request.at("height")))
        fail("Native capture requires supported version and positive representable dimensions");
    constexpr std::uint64_t max_pixels = 16 * 1024 * 1024;
    if (request.at("width").get<std::uint64_t>() * request.at("height").get<std::uint64_t>() > max_pixels)
        fail("Native capture exceeds the total pixel limit");
    for (const char* key : {"plates", "objects"}) {
        const auto& values = request.at(key);
        if (!values.is_array())
            fail("Native capture selections must be arrays");
        std::set<int> seen;
        for (const auto& value : values)
            if (!positive_index(value) || !seen.insert(value.get<int>()).second)
                fail("Native capture selections require distinct positive indices");
    }
    const auto& visibility = request.at("visibility");
    for (const char* key : {"beds", "plate_identifiers", "prime_towers"})
        if (!visibility.at(key).is_boolean())
            fail("Native capture visibility controls must be booleans");
    const auto& camera = request.at("camera");
    const auto preset = camera.at("preset").get<std::string>();
    if (preset != "isometric" && preset != "top" && preset != "front" && preset != "rear" &&
        preset != "left" && preset != "right" && preset != "bottom")
        fail("Unsupported native camera preset");
    const auto projection = camera.at("projection").get<std::string>();
    if (projection != "orthographic" && projection != "perspective")
        fail("Unsupported native capture projection");
    const double margin = camera.at("margin").get<double>();
    if (!std::isfinite(margin) || margin < 1)
        fail("Automatic framing requires a finite margin of at least one");
    if (camera.contains("zoom") && !camera.at("zoom").is_null()) {
        const double zoom = camera.at("zoom").get<double>();
        if (!std::isfinite(zoom) || zoom <= 0)
            fail("Native camera zoom must be finite and positive");
    }
    for (const char* key : {"orientation", "target"}) {
        if (!camera.contains(key) || camera.at(key).is_null())
            continue;
        const auto& value = camera.at(key);
        if (!value.is_array() || value.size() != (std::string(key) == "orientation" ? 4 : 3))
            fail("Native camera vector has an invalid dimension");
        double squared_norm = 0;
        for (const auto& coordinate : value) {
            const double number = coordinate.get<double>();
            if (!std::isfinite(number))
                fail("Native camera vectors must be finite");
            squared_norm += number * number;
        }
        if (std::string(key) == "orientation" && (squared_norm == 0 || !std::isfinite(squared_norm)))
            fail("Native camera orientation requires a finite nonzero quaternion");
    }
}

}}
