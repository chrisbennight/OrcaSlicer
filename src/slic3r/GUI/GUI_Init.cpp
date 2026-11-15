#include "GUI_Init.hpp"
#include "WorkspaceCaptureRequest.hpp"

#include "libslic3r/AppConfig.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/InstanceCheck.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

// To show a message box if GUI initialization ends up with an exception thrown.
#include <wx/msgdlg.h>
#include <wx/modalhook.h>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>

#if __APPLE__
    #include <signal.h>
#endif // __APPLE__

namespace Slic3r {
namespace GUI {

const std::vector<std::pair<int, int>> OpenGLVersions::core    = { {3,2}, {3,3}, {4,0}, {4,1}, {4,2}, {4,3}, {4,4}, {4,5}, {4,6} };

int GUI_Run(GUI_InitParams &params)
{
#if __APPLE__
    // On OSX, we use boost::process::spawn() to launch new instances of PrusaSlicer from another PrusaSlicer.
    // boost::process::spawn() sets SIGCHLD to SIGIGN for the child process, thus if a child PrusaSlicer spawns another
    // subprocess and the subrocess dies, the child PrusaSlicer will not receive information on end of subprocess
    // (posix waitpid() call will always fail).
    // https://jmmv.dev/2008/10/boostprocess-and-sigchld.html
    // The child instance of PrusaSlicer has to reset SIGCHLD to its default, so that posix waitpid() and similar continue to work.
    // See GH issue #5507
    signal(SIGCHLD, SIG_DFL);
#endif // __APPLE__

    boost::filesystem::path capture_data;
    Slic3r::ScopeGuard cleanup_capture_data([&]() {
        if (!capture_data.empty()) {
            boost::system::error_code error;
            boost::filesystem::current_path(params.capture_output_directory, error);
            if (error)
                BOOST_LOG_TRIVIAL(error) << "Could not restore the capture working directory: " << error.message();
            error.clear();
            boost::filesystem::remove_all(capture_data, error);
            if (error)
                BOOST_LOG_TRIVIAL(error) << "Could not remove disposable capture configuration: " << error.message();
        }
    });
    struct CaptureDialogHook : wxModalDialogHook {
        GUI_InitParams& params;
        explicit CaptureDialogHook(GUI_InitParams& value) : params(value) {}
        int Enter(wxDialog*) override
        {
            params.capture_requires_interaction = true;
            return wxID_CANCEL;
        }
    } dialogs(params);
    if (!params.capture_workspace.empty())
        dialogs.Register();
    try {
        if (!params.capture_workspace.empty()) {
            params.capture_workspace = boost::filesystem::canonical(params.capture_workspace).string();
            boost::nowide::ifstream request(params.capture_workspace);
            validate_workspace_capture_request(nlohmann::json::parse(request));
            params.capture_output_directory = boost::filesystem::current_path().string();
            for (const char* name : {"capture.png", "capture.json"})
                if (boost::filesystem::exists(boost::filesystem::path(params.capture_output_directory) / name))
                    throw std::runtime_error("Workspace capture output already exists; use a fresh output directory");
            for (auto& source : params.input_files)
                source = boost::filesystem::canonical(source).string();
            const auto directory = boost::filesystem::temp_directory_path() /
                boost::filesystem::unique_path("orca-capture-%%%%-%%%%-%%%%-%%%%");
            if (!boost::filesystem::create_directory(directory))
                throw std::runtime_error("Could not create disposable capture configuration");
            capture_data = directory;
            boost::filesystem::permissions(capture_data, boost::filesystem::owner_all);
            // Never load or change an interactive user's application profile.
            Slic3r::set_data_dir(capture_data.string());
        }
        //GUI::GUI_App* gui = new GUI::GUI_App(params.start_as_gcodeviewer ? GUI::GUI_App::EAppMode::GCodeViewer : GUI::GUI_App::EAppMode::Editor);
        GUI::GUI_App* gui = new GUI::GUI_App();
        //if (gui->get_app_mode() != GUI::GUI_App::EAppMode::GCodeViewer) {
            // G-code viewer is currently not performing instance check, a new G-code viewer is started every time.
            bool gui_single_instance_setting = gui->app_config->get("app", "single_instance") == "true";
            if (params.capture_workspace.empty() && Slic3r::instance_check(params.argc, params.argv, gui_single_instance_setting)) {
                //TODO: do we have delete gui and other stuff?
                return -1;
            }
        //}

//      gui->autosave = m_config.opt_string("autosave");
        GUI::GUI_App::SetInstance(gui);
        gui->init_params = &params;
        if (!params.capture_workspace.empty()) {
            gui->app_config->set("show_splash_screen", "false");
            gui->app_config->set("background_processing", "0");
            gui->app_config->set("installed_networking", "false");
            gui->app_config->set("update_network_plugin", "false");
            gui->app_config->set("sync_system_preset", "false");
            gui->app_config->set("sync_user_preset", "false");
            gui->app_config->set("stealth_mode", "true");
            gui->app_config->set("no_warn_when_modified_gcodes", "true");
            gui->app_config->set("show_hints", "false");
        }

        if (params.argc > 1) {
            // STUDIO-273 wxWidgets report error when opening some files with specific names
            // wxWidgets does not handle parameters, so intercept parameters here, only keep the app name
            int                 argc = 1;
            std::vector<char *> argv;
            argv.push_back(params.argv[0]);
            const int result = wxEntry(argc, argv.data());
            return params.capture_workspace.empty() ? result : params.capture_exit_code;
        } else {
            return wxEntry(params.argc, params.argv);
        }
    } catch (const Slic3r::Exception &ex) {
        BOOST_LOG_TRIVIAL(error) << ex.what() << std::endl;
        if (!params.capture_workspace.empty())
            return 1;
        wxMessageBox(boost::nowide::widen(ex.what()), _L("Orca Slicer GUI initialization failed"), wxICON_STOP);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << ex.what() << std::endl;
        if (!params.capture_workspace.empty())
            return 1;
        wxMessageBox(format_wxstr(_L("Fatal error, exception caught: %1%"), ex.what()), _L("Orca Slicer GUI initialization failed"), wxICON_STOP);
    }
    // error
    return 1;
}
}
}
