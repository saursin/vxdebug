#include <iostream>
#include <string>

#include <argparse.h>
#include "logger.h"
#include "vxdebug.h"

#ifndef VXDBG_VERSION
    #define VXDBG_VERSION "v0.1"
#endif

std::string banner = 
"+--------------------------------------------------------------------------+\n"
"| Vortex Debugger                                                          |\n"
"| Copyright © 2019-2023                                                    |\n"
"|                                                                          |\n"
"| Licensed under the Apache License, Version 2.0 (the \"License\");          |\n"
"| you may not use this file except in compliance with the License.         |\n"
"| You may obtain a copy of the License at                                  |\n"
"| http://www.apache.org/licenses/LICENSE-2.0                               |\n"
"|                                                                          |\n"
"| Unless required by applicable law or agreed to in writing, software      |\n"
"| distributed under the License is distributed on an \"AS IS\" BASIS,        |\n"
"| WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. |\n"
"| See the License for the specific language governing permissions and      |\n"
"| limitations under the License.                                           |\n"
"+--------------------------------------------------------------------------+\n";


int main(const int argc, char** argv) {
    ArgParse::ArgumentParser parser("vxdbg", "Vortex Debugger");
    parser.add_argument({"-s", "--script"}, "Script file to execute", ArgParse::STR, "");
    parser.add_argument({"--log"}, "Log file path", ArgParse::STR, "");
    parser.add_argument({"-v", "--verbose"}, "Set verbosity (0:err, 1:warn, 2:info, 3-9:debug)", ArgParse::INT, "2");
    parser.add_argument({"--version"}, "Show version information and exit", ArgParse::BOOL, "false");
    parser.add_argument({"--no-banner"}, "Do not print banner", ArgParse::BOOL, "false");
    parser.add_argument({"--no-color"}, "Disable colored output", ArgParse::BOOL, "false");
    parser.add_argument({"--no-cli"}, "Disable interactive CLI", ArgParse::BOOL, "false");
    
    int rc;

    // Parse arguments
    rc = parser.parse_args(argc, argv);
    if (rc != 0) {
        return rc;
    }

    // Handle --version
    if (parser.get<bool>("version")) {
        std::cout << "Vortex Debugger " << VXDBG_VERSION << std::endl;
        return 0;
    }

    // Setup Logger 
    if (parser.get<bool>("no_color")) {
        Logger::set_color_enabled(false);
    }
    int verbosity = parser.get<int>("verbose");
    Logger::set_global_level(static_cast<LogLevel>(verbosity));
    Logger::set_global_debug_threshold(verbosity);

    std::string log_file = parser.get<std::string>("log");
    if (!log_file.empty()) {
        Logger::ginfo("Logging to file: " + log_file);
        Logger::set_output_file(log_file);
    }

    // Print banner
    if (!parser.get<bool>("no_banner")) {
        std::cout << ANSI_YLW << banner << ANSI_RST;
    }

    try {
        // Create debugger instance
        Logger::ginfo("Starting Vortex Debugger " VXDBG_VERSION);
        VortexDebugger debugger;

        // Execute script if provided
        std::string script = parser.get<std::string>("script");
        if (!script.empty()) {
            rc = debugger.execute_script(script);
            if (rc != 0) {
                Logger::gerror("Script execution failed with code " + std::to_string(rc));
                return rc;
            }
        }
        
        // Start interactive CLI unless disabled or exited from script
        if (!parser.get<bool>("no_cli") && debugger.get_state() != EXIT) {
            rc = debugger.start_cli();
            if (rc != 0) {
                Logger::gerror("CLI exited with code " + std::to_string(rc));
            }
        }
    } catch (const std::exception& e) {
        // Catch-all for unexpected errors
        Logger::gerror("Fatal error: " + std::string(e.what()));
    }
    return rc;
}