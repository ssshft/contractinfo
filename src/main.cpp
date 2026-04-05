#include <iostream>
#include <csignal>
#include "log_engine.h"
#include "program_util.h"
#include "config.h"
#include "crypto_exception.h"
#include "operation/ContractInfoOperation.h"



static void signal_handler(int signum) {
    LOG_INFO("handler interrupt signal {} received, ContractInfo will exit now.", signum);
    fprintf(stdout, "handler interrupt signal (%d) received.\n", signum);
    fmtlog::poll();
    sleep(0.5);//wait logger write data to disk
    exit(signum);
}

static void usage(void){
    fprintf(stderr, "\nusage:\n");
    fprintf(stderr, "./application json_config_file \n");
    fprintf(stderr, "\n");
    exit(-1);
}

int main(int argc, char* argv[]) {
    if ( argc != 2 ){
        usage();
    }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    Config* config = Config::instance();
    config->load(argv[1]);

    std::string tag = "";
    std::string logPath = "";
    std::string logLevel = "";

    if (config->get_string("tag", tag) && config->get_log_config("log_path", logPath) && config->get_log_config("level", logLevel)) {
        std::cout << "tag: " << tag << " logPath: " << logPath << " level: " << logLevel << std::endl;
        if (crypto::create_directory(logPath)) {
            log_maintain(tag, logPath, logLevel);
        }
        else {
            exit(-1);
        }
    }
    else {
        fprintf(stderr, "not found log config info in %s,\nneed to add log config\n", argv[1]);
        exit(-1);
    }

    std::string program = tag;
    long currentPid = getpid();
    long filePid = crypto::get_program_pid(program);
    if(!crypto::ensure_one_instance(program) && currentPid != filePid) {
        std::string errormsg = tag + " with pid=" + std::to_string(filePid) + " already exists, aborted";
        cryptothrow(errormsg.c_str(), -1);
    }
    crypto::write_program_pid(program);

    ContractInfoOperation* op = new ContractInfoOperation();
    if(op->preStart(config)) {
        op->run();
    }
    else {
        cryptothrow("ContractInfo start failed!", -1);
    }

    while(1) {
        log_maintain(tag, logPath, logLevel);
        usleep(1000);
    }

    return 0;
}