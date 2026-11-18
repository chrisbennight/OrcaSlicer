#include <glad/gl.h>

#include "WorkspaceCapture.hpp"

#include "BuildCommit.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI_Init.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "nlohmann/json.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r { namespace GUI {
namespace {
using nlohmann::json;

Vec3d vector3(const json& value)
{
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error("Camera vectors require three finite coordinates");
    Vec3d result;
    for (size_t i = 0; i != 3; ++i) {
        result[i] = value.at(i).get<double>();
        if (!std::isfinite(result[i]))
            throw std::runtime_error("Camera vectors require finite coordinates");
    }
    return result;
}

json vector_json(const Vec3d& value)
{
    return {value.x(), value.y(), value.z()};
}

json matrix_json(const Transform3d& transform)
{
    json matrix = json::array();
    for (int row = 0; row != 4; ++row) {
        json values = json::array();
        for (int column = 0; column != 4; ++column)
            values.push_back(transform.matrix()(row, column));
        matrix.push_back(values);
    }
    return matrix;
}

void check_gl_error(const char* stage)
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error(std::string("Native workspace ") + stage +
            " failed (OpenGL error " + std::to_string(error) + ")");
}

std::set<int> selection(const json& request, const char* key, int count)
{
    std::set<int> result;
    for (const auto& value : request.at(key)) {
        const int index = value.get<int>();
        if (index < 1 || index > count || !result.insert(index - 1).second)
            throw std::runtime_error(std::string("Invalid or duplicate selection: ") + key);
    }
    return result;
}

// Capture owns these objects only while the application's GL context is current.
struct Framebuffer {
    GLint previous_framebuffer { 0 };
    GLint previous_renderbuffer { 0 };
    GLint previous_pack_alignment { 4 };
    GLuint framebuffer { 0 }, color { 0 }, depth { 0 };

    Framebuffer()
    {
        if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
            throw WorkspaceCaptureUnavailable("Native workspace capture requires core/ARB framebuffer support");
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_renderbuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &color);
        glGenRenderbuffers(1, &depth);
    }

    ~Framebuffer()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, previous_renderbuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
        glDeleteRenderbuffers(1, &depth);
        glDeleteRenderbuffers(1, &color);
        glDeleteFramebuffers(1, &framebuffer);
    }

    void allocate(int width, int height)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        const GLenum attachment = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &attachment);
        glReadBuffer(attachment);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Native capture framebuffer is unavailable");
    }
};
}

void capture_workspace(const std::string& request_json, const std::string& output_directory)
{
    const json request = json::parse(request_json);
    const int width = request.at("width").get<int>();
    const int height = request.at("height").get<int>();
    if (width <= 0 || height <= 0 || size_t(width) > std::numeric_limits<size_t>::max() / size_t(height) / 3)
        throw std::runtime_error("Native capture dimensions are not representable");

    Plater& plater = *wxGetApp().plater();
    GLCanvas3D& canvas = *plater.canvas3D();
    if (!canvas.make_current_for_postinit() || !wxGetApp().init_opengl() || !canvas.is_initialized())
        throw WorkspaceCaptureUnavailable("Native capture OpenGL context is unavailable");
    PartPlateList& plates = plater.get_partplate_list();
    // Rebuild after resolving the printer. Placements without a saved plate
    // use geometry, while explicitly assigned instances keep their source plate.
    plates.reload_all_objects(false, -1, true);
    canvas.reload_scene(true);

    GLint max_size = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    if (width > max_size || height > max_size)
        throw std::runtime_error("Native capture dimensions exceed the graphics backend's renderbuffer size");

    auto selected_plates = selection(request, "plates", plates.get_plate_count());
    const auto selected_objects = selection(request, "objects", int(plater.model().objects.size()));
    if (selected_plates.empty())
        for (int i = 0; i != plates.get_plate_count(); ++i)
            selected_plates.insert(i);
    const auto& visibility = request.at("visibility");
    const bool show_beds = visibility.at("beds").get<bool>();
    const bool show_identifiers = visibility.at("plate_identifiers").get<bool>();
    const bool show_towers = visibility.at("prime_towers").get<bool>();
    const bool all_plates = request.at("plates").empty();
    auto visible = [&](const GLVolume& volume) {
        if (volume.is_modifier)
            return false;
        const int object = volume.composite_id.object_id;
        if (volume.is_wipe_tower)
            return show_towers && selected_plates.count(object - 1000) != 0;
        return (selected_objects.empty() || selected_objects.count(object) != 0) &&
            (all_plates || selected_plates.count(plates.find_instance(object, volume.composite_id.instance_id)) != 0);
    };

    BoundingBoxf3 bounds;
    const bool show_current_bed = show_beds && selected_plates.count(plates.get_curr_plate_index()) != 0;
    if (show_current_bed) {
        // Native bed meshes load lazily during rendering. Initialize the mesh
        // before fitting the camera to its full bounds.
        canvas.render_workspace_bed();
        bounds.merge(canvas.workspace_bed_bounding_box());
    }
    json plate_ids = json::array(), volumes = json::array();
    json plate_bed_overrides = json::object();
    for (int index : selected_plates) {
        plate_ids.push_back(index + 1);
        PartPlate* plate = plates.get_plate(index);
        const BedType bed = plate->get_bed_type();
        if (bed != btDefault)
            plate_bed_overrides[std::to_string(index + 1)] = bed_type_to_gcode_string(bed);
        if (show_beds || show_identifiers)
            bounds.merge(plate->get_capture_bounding_box(show_beds, show_identifiers));
    }
    for (const GLVolume* volume : canvas.get_volumes().volumes) {
        if (!visible(*volume))
            continue;
        const auto box = volume->transformed_bounding_box();
        bounds.merge(box);
        volumes.push_back({{"object", volume->composite_id.object_id + 1},
            {"instance", volume->composite_id.instance_id + 1},
            {"prime_tower", volume->is_wipe_tower}, {"world_matrix", matrix_json(volume->world_matrix())},
            {"material_color", {volume->color.r(), volume->color.g(), volume->color.b(), volume->color.a()}},
            {"bounds_mm", {{"min", vector_json(box.min)}, {"max", vector_json(box.max)}}}});
    }
    if (!bounds.defined)
        throw std::runtime_error("No native layout content matches the capture selection");

    Camera& camera = plater.get_camera();
    const Camera previous_camera = camera;
    Slic3r::ScopeGuard restore_camera([&]() { camera = previous_camera; camera.apply_viewport(); });
    camera.enable_update_config_on_type_change(false);
    camera.requires_zoom_to_bed = false;
    camera.requires_zoom_to_volumes = false;
    camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_PLATE_IDLE;
    const auto& view = request.at("camera");
    const std::string projection = view.at("projection").get<std::string>();
    if (projection != "orthographic" && projection != "perspective")
        throw std::runtime_error("Unsupported native capture projection");
    camera.set_scene_box(bounds);
    camera.set_viewport(0, 0, width, height);
    const std::string preset = view.at("preset").get<std::string>();
    if (preset == "isometric")
        camera.select_view(Camera::ViewAngleType::Iso);
    else if (preset == "top" || preset == "front" || preset == "rear" || preset == "left" || preset == "right" || preset == "bottom")
        camera.select_view(preset);
    else
        throw std::runtime_error("Unsupported native camera preset");
    if (view.contains("orientation") && !view.at("orientation").is_null()) {
        const auto& value = view.at("orientation");
        if (!value.is_array() || value.size() != 4)
            throw std::runtime_error("Camera orientation requires a w/x/y/z quaternion");
        Eigen::Quaterniond orientation(value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>(), value.at(3).get<double>());
        if (!orientation.coeffs().allFinite() || !std::isfinite(orientation.norm()) || orientation.norm() == 0)
            throw std::runtime_error("Camera orientation must be finite and nonzero");
        orientation.normalize();
        Transform3d rotation = Transform3d::Identity();
        rotation.linear() = orientation.toRotationMatrix();
        camera.set_rotation(rotation);
    }
    // Axis presets may choose an automatic projection. The explicit request
    // wins after the preset and custom orientation have been applied.
    camera.set_type(projection == "orthographic" ? Camera::EType::Ortho : Camera::EType::Perspective);
    const double margin = view.at("margin").get<double>();
    if (!std::isfinite(margin) || margin < 1)
        throw std::runtime_error("Automatic capture framing requires a finite margin of at least one");
    camera.zoom_to_box(bounds, margin);
    // A deterministic distance keeps perspective framing and resolved-camera
    // replay independent of the interactive camera left by project loading.
    const Vec3d center = bounds.center();
    const double distance = std::max(1000.0, 2.0 * bounds.size().norm());
    camera.look_at(center - distance * camera.get_dir_forward(), center, camera.get_dir_up());
    camera.set_zoom(camera.get_zoom());
    const bool custom_target = view.contains("target") && !view.at("target").is_null();
    const bool custom_zoom = view.contains("zoom") && !view.at("zoom").is_null();
    if (!custom_target && !custom_zoom) {
        // The ordinary native fit is planar. Measure all corners through the
        // actual projection so perspective magnification also fits the margin.
        if (!camera.apply_projection(bounds))
            throw std::runtime_error("Native camera cannot frame the layout bounds");
        const Eigen::Matrix4d matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        double extent = 0;
        for (int corner = 0; corner != 8; ++corner) {
            Eigen::Vector4d point;
            for (int axis = 0; axis != 3; ++axis)
                point[axis] = corner & (1 << axis) ? bounds.max[axis] : bounds.min[axis];
            point[3] = 1;
            const Eigen::Vector4d projected = matrix * point;
            if (!projected.allFinite() || projected[3] <= 0)
                throw std::runtime_error("Native camera cannot frame the layout bounds");
            extent = std::max(extent, std::max(std::abs(projected[0]), std::abs(projected[1])) / projected[3]);
        }
        if (extent * margin > 1.0) {
            const double fitted_zoom = camera.get_zoom() / (extent * margin);
            camera.set_zoom(fitted_zoom);
            if (camera.get_zoom() > fitted_zoom * (1.0 + 1e-9))
                throw std::runtime_error("Native camera zoom limits cannot fit the requested margin");
        }
    }
    if (view.contains("target") && !view.at("target").is_null())
        camera.set_target(vector3(view.at("target")));
    if (view.contains("zoom") && !view.at("zoom").is_null()) {
        const double zoom = view.at("zoom").get<double>();
        if (!std::isfinite(zoom) || zoom <= 0)
            throw std::runtime_error("Camera zoom must be finite and positive");
        camera.set_zoom(zoom);
        if (std::abs(camera.get_zoom() - zoom) > 1e-9 * std::max(1.0, zoom))
            throw std::runtime_error("Requested native camera zoom exceeds the layout's supported range");
    }
    camera.apply_viewport();
    if (!camera.apply_projection(bounds))
        throw std::runtime_error("Native camera cannot project the requested view");

    // The interactive GUI shares this context and may leave errors from its
    // toolbar drawing. Capture checks only its own framebuffer and draw calls.
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError())
        BOOST_LOG_TRIVIAL(warning) << "Discarding an earlier GUI OpenGL error before capture: " << error;
    Framebuffer framebuffer;
    framebuffer.allocate(width, height);
    check_gl_error("framebuffer allocation");
    glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    const bool bottom = camera.get_dir_forward().z() > 0;
    if (show_current_bed)
        canvas.render_workspace_bed();
    plates.render(camera.get_view_matrix(), camera.get_projection_matrix(), bottom,
        false, true, -1, false, true, false, false, &selected_plates, show_identifiers, show_beds, false);
    check_gl_error("plate rendering");
    canvas.render_workspace_volumes(visible);
    check_gl_error("model rendering");
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    check_gl_error("pixel readback");
    const size_t row_bytes = size_t(width) * 3;
    for (int row = 0; row < height / 2; ++row)
        std::swap_ranges(pixels.begin() + size_t(row) * row_bytes,
            pixels.begin() + size_t(row + 1) * row_bytes,
            pixels.begin() + size_t(height - row - 1) * row_bytes);
    const auto image_path = boost::filesystem::path(data_dir()) / "capture.png";
    const auto metadata_path = boost::filesystem::path(data_dir()) / "capture.json";
    if (!png::write_rgb_to_file(image_path.string(), width, height, pixels))
        throw std::runtime_error("Could not write the native capture PNG");

    const auto& rotation = camera.get_view_rotation();
    json result = {{"format_version", 1}, {"kind", "native_orca_layout"},
        {"representation", "model_placement"}, {"prime_towers", "native_estimated"},
        {"engine", {{"name", "OrcaSlicer"}, {"version", SoftFever_VERSION}, {"source_revision", build_commit_hash}}},
        {"width", width}, {"height", height}, {"plates", plate_ids}, {"objects", request.at("objects")},
        {"plate_bed_overrides", plate_bed_overrides},
        {"visibility", visibility}, {"camera", {{"preset", preset}, {"projection", projection},
            {"orientation", {rotation.w(), rotation.x(), rotation.y(), rotation.z()}},
            {"target", vector_json(camera.get_target())}, {"zoom", camera.get_zoom()}, {"margin", margin}}},
        {"bounds_mm", {{"min", vector_json(bounds.min)}, {"max", vector_json(bounds.max)}}},
        {"view_matrix", matrix_json(camera.get_view_matrix())},
        {"projection_matrix", matrix_json(camera.get_projection_matrix())},
        {"volumes", volumes}, {"graphics_renderer", reinterpret_cast<const char*>(glGetString(GL_RENDERER))}};
    boost::nowide::ofstream output(metadata_path.string());
    output << result.dump(2);
    output.close();
    if (!output)
        throw std::runtime_error("Could not write native capture metadata");
    // Exclusive copies enforce preservation even if another process creates an
    // output after startup validation. Encoding stays in the private data root.
    const auto published_image = boost::filesystem::path(output_directory) / "capture.png";
    boost::filesystem::copy_file(image_path, published_image);
    try {
        boost::filesystem::copy_file(metadata_path, boost::filesystem::path(output_directory) / "capture.json");
    } catch (...) {
        boost::system::error_code error;
        boost::filesystem::remove(published_image, error);
        if (error)
            BOOST_LOG_TRIVIAL(error) << "Could not remove incomplete workspace capture: " << error.message();
        throw;
    }
}

}}
