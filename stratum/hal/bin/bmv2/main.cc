/* Copyright 2018-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bm/bm_sim/options_parse.h"
#include "bm/simple_switch/runner.h"
#include "bm/bm_sim/logger.h"

#include "gflags/gflags.h"
#include "PI/frontends/proto/device_mgr.h"
#include "PI/frontends/proto/logging.h"
#include "stratum/glue/init_google.h"
#include "stratum/glue/logging.h"
#include "stratum/hal/lib/common/hal.h"
#include "stratum/hal/lib/phal/phal_sim.h"
#include "stratum/hal/lib/bmv2/bmv2_switch.h"
#include "stratum/lib/security/auth_policy_checker.h"
#include "stratum/lib/security/credentials_manager.h"

DEFINE_string(initial_pipeline, "stratum/hal/bin/bmv2/dummy.json",
              "Path to initial pipeline for BMv2 (required for starting BMv2)");
DEFINE_uint32(device_id, 1,
              "BMv2 device/node id");
DEFINE_uint32(cpu_port, 64,
              "BMv2 port number for CPU port (used for packet I/O)");
DEFINE_bool(console_logging, true,
              "Log BMv2 message to console.");
DEFINE_string(bmv2_log_level, "info",
              "Log level of Bmv2(trace, debug, info, warn, error, off)");

using ::pi::fe::proto::DeviceMgr;

namespace stratum {
namespace hal {
namespace bmv2 {

std::unordered_map<std::string, bm::Logger::LogLevel> log_level_map = {
    {"trace", bm::Logger::LogLevel::TRACE},
    {"debug", bm::Logger::LogLevel::DEBUG},
    {"info", bm::Logger::LogLevel::INFO},
    {"warn", bm::Logger::LogLevel::WARN},
    {"error", bm::Logger::LogLevel::ERROR},
    {"off", bm::Logger::LogLevel::OFF}};

void ParseInterfaces(int argc, char* argv[], bm::OptionsParser& parser) {
  for (int i = 1; i < argc; i++) {
    char* intf;
    if ((intf = strchr(argv[i], '@')) != nullptr) {
      // Found an interface
      int intf_num = strtol(argv[i], &intf, 10);
      intf += 1; // Point to the start of the interface name
      LOG(INFO) << "Parsed intf from command line: port " << intf_num
                << " -> " << intf;
      parser.ifaces.add(intf_num, intf);
      LOG(WARNING) << "Providing interfaces on the command-line is deprecated, "
                   << "and you will not be able to perform gNMI RPCs to "
                   << "modify port config or access port state; "
                   << "please use --chassis_config_file instead";
    }
    else {
      LOG(ERROR) << "Ignoring extraneous non-option argument: " << argv[i];
    }
  }
}

int Main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);
  InitHerculesLogging();

  DeviceMgr::init(256 /* max devices */);

  using ::bm::sswitch::SimpleSwitchRunner;

  // Build BMv2 parser from command line values
  bm::OptionsParser parser;
  parser.console_logging = FLAGS_console_logging;
  // We need a "starting" P4 pipeline otherwise init_and_start() will block
  // TODO(antonin): figure out how to package the file with the binary
  parser.config_file_path = FLAGS_initial_pipeline;
  parser.device_id = FLAGS_device_id;

  // Sets up bmv2 log level
  auto log_level_it = log_level_map.find(FLAGS_bmv2_log_level);
  if (log_level_it == log_level_map.end()) {
    LOG(WARNING) << "Invalid value " << FLAGS_bmv2_log_level << " for -bmv2_log_level\n"
              << "Run with -help to see possible values\n";
    parser.log_level = bm::Logger::LogLevel::INFO;
  } else {
    parser.log_level = log_level_it->second;
  }

  // TODO(antonin): There may be a better way to parse the interface list
  // (e.g. it can be done with OptionsParser::parse)
  ParseInterfaces(argc, argv, parser);

  SimpleSwitchRunner *runner = new SimpleSwitchRunner(FLAGS_cpu_port);
  {
    using ::pi::fe::proto::LogWriterIface;
    using ::pi::fe::proto::LoggerConfig;

    class P4RuntimeLogger : public LogWriterIface {
      void write(Severity severity, const char *msg) override {
        auto severity_map = [&severity]() {
          namespace spdL = spdlog::level;
          switch (severity) {
            case Severity::TRACE : return spdL::trace;
            case Severity::DEBUG: return spdL::debug;
            case Severity::INFO: return spdL::info;
            case Severity::WARN: return spdL::warn;
            case Severity::ERROR: return spdL::err;
            case Severity::CRITICAL: return spdL::critical;
          }
          return spdL::off;
        };
        // TODO(antonin): use stratum logger instead
        bm::Logger::get()->log(severity_map(), "[P4Runtime] {}", msg);
      }
    };
    LoggerConfig::set_writer(std::make_shared<P4RuntimeLogger>());
  }
  LOG(ERROR) << "Starting bmv2 simple_switch and waiting for P4 pipeline";
  // blocks until a P4 pipeline is set
  {
    int status = runner->init_and_start(parser);
    if (status != 0) {
      LOG(ERROR) << "Error when starting bmv2 simple_switch";
      return status;
    }
  }

  int unit(0);
  // bmv2 needs to know the actual device_id at instantiation time, so we cannot
  // wait until PushChassisConfig.
  uint64 node_id(FLAGS_device_id);
  std::unique_ptr<DeviceMgr> device_mgr(new DeviceMgr(node_id));

  auto pi_node = pi::PINode::CreateInstance(device_mgr.get(), unit);
  auto* phal_sim = PhalSim::CreateSingleton();
  std::map<uint64, SimpleSwitchRunner*> node_id_to_bmv2_runner = {
    {node_id, runner},
  };
  auto bmv2_chassis_manager = Bmv2ChassisManager::CreateInstance(
      phal_sim, node_id_to_bmv2_runner);
  std::map<uint64, pi::PINode*> node_id_to_pi_node = {
    {node_id, pi_node.get()},
  };
  auto pi_switch = Bmv2Switch::CreateInstance(
      phal_sim, bmv2_chassis_manager.get(), node_id_to_pi_node);

  // Create the 'Hal' class instance.
  auto auth_policy_checker = AuthPolicyChecker::CreateInstance();
  auto credentials_manager = CredentialsManager::CreateInstance();
  auto* hal = Hal::CreateSingleton(stratum::hal::OPERATION_MODE_SIM,
                                   pi_switch.get(),
                                   auth_policy_checker.get(),
                                   credentials_manager.get());
  if (!hal) {
    LOG(ERROR) << "Failed to create the Hercules Hal instance.";
    return -1;
  }

  // Setup and start serving RPCs.
  // TODO(antonin): currently this fails because persistent_config_dir flag is
  // not set. Need to figure out if this is needed and if not how to circumvent
  // the error.
  ::util::Status status = hal->Setup();
  if (!status.ok()) {
    LOG(ERROR)
        << "Error when setting up Hercules HAL (but we will continue running): "
        << status.error_message();
  }
  status = hal->Run();  // blocking
  if (!status.ok()) {
    LOG(ERROR) << "Error when running Hercules HAL: " << status.error_message();
    return -1;
  }

  LOG(INFO) << "See you later!";
  return 0;
}

}  // namespace bmv2
}  // namespace hal
}  // namespace stratum

int
main(int argc, char* argv[]) {
  return stratum::hal::bmv2::Main(argc, argv);
}