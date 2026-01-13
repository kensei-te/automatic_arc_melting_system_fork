#include "main_process/process_control.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>
#include <regex>
#include <map>
#include <unistd.h>
#include <limits.h>


struct LoopFrame {
    int id;
    int repeat;
    std::vector<std::string> block;
};

static bool parseLoopStart(const std::string& line, int& id, int& repeat) {
    static const std::regex re(R"(^\s*loop(\d+)_(\d+)\s*$)");
    std::smatch m;
    if (!std::regex_match(line, m, re)) return false;
    id = std::stoi(m[1].str());
    repeat = std::stoi(m[2].str());
    return true;
}

static bool parseLoopEnd(const std::string& line, int& id) {
    static const std::regex re(R"(^\s*loop(\d+)_end\s*$)");
    std::smatch m;
    if (!std::regex_match(line, m, re)) return false;
    id = std::stoi(m[1].str());
    return true;
}

std::vector<std::string> compileSequence(const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    std::vector<LoopFrame> stack;

    for (const auto& line : raw) {
        int id = 0, rep = 0;

        if (parseLoopStart(line, id, rep)) {
            if (rep <= 0) {
                throw std::runtime_error("loop repeat must be > 0: " + line);
            }
            stack.push_back(LoopFrame{id, rep, {}});
            continue;
        }

        if (parseLoopEnd(line, id)) {
            if (stack.empty()) {
                throw std::runtime_error("loop_end without loop start: " + line);
            }
            auto frame = stack.back();
            stack.pop_back();

            if (frame.id != id) {
                throw std::runtime_error("loop id mismatch: expected loop" +
                    std::to_string(frame.id) + "_end, got " + line);
            }

            // repeat-expand this frame.block
            std::vector<std::string> expanded;
            expanded.reserve(frame.block.size() * frame.repeat);
            for (int i = 0; i < frame.repeat; ++i) {
                expanded.insert(expanded.end(), frame.block.begin(), frame.block.end());
            }

            if (stack.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
            } else {
                stack.back().block.insert(stack.back().block.end(), expanded.begin(), expanded.end());
            }
            continue;
        }

        // normal instruction line
        if (stack.empty()) out.push_back(line);
        else stack.back().block.push_back(line);
    }

    if (!stack.empty()) {
        throw std::runtime_error("unclosed loop: loop" + std::to_string(stack.back().id) + "_...");
    }

    return out;
}

ProcessController::ProcessController(std::string command)
    : current_step_(command), 
      step_index_(0){
    
    // Initialize device state manager
    devices_manager_.initializing = true;
    devices_manager_.addDevice("weighing");
    devices_manager_.addDevice("slider");
    devices_manager_.addDevice("cobotta");
    devices_manager_.addDevice("plc");
    devices_manager_.initializing = false;
    
    // Initialize process sequences
    initializeSequences();
    moveToNextStep();
}

std::string ProcessController::updateDeviceStatuses(const std::string& command) {
    bool message = devices_manager_.updateDeviceStatus(command);
    if (message) {
        return "update device status success";
    } else {
        return "update device status error";
    }
}

void ProcessController::initializeSequences() {
    std::vector<std::string> raw;
    
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        std::cerr << "Current working directory: " << cwd << std::endl;
    } else {
        std::cerr << "Failed to get current directory" << std::endl;
    }
    
    // Load sequences from file or define them directly
    // try to load from file
    std::ifstream file(secquence_file_);
    
    if (!file.is_open()) {
        std::cerr << "Error opening file: " << secquence_file_ << std::endl;
        raw = {
            "slider_init cobotta_init weighing_init plc_init",
            "slider_shelf_1 plc_buzz",
            "weighing_open slider_weight_pos cobotta_test",
            "slider_init cobotta_init weighing_init plc_init"
            "finished"
        };
        return;
    }
    else {
        std::cerr << "load process from file: " << secquence_file_ << std::endl;
        std::string line;
        while (std::getline(file, line)) {
            // if line is empty or a comment, skip it
            if (line.empty() || line[0] == '#') {
                continue;
            }
            raw.push_back(line);
        }
        file.close();
        // sequence_.push_back("finished");
    }

    // 1) compile
    try {
        sequence_ = compileSequence(raw);
    } catch (const std::exception& e) {
        std::cerr << "Sequence compile error: " << e.what() << std::endl;
        sequence_.clear();
        sequence_.push_back("finished");
    }

    // 2) guarantee finished
    if (sequence_.empty() || sequence_.back() != "finished") {
        sequence_.push_back("finished");
    }

}

std::string ProcessController::getCurrentStep() const {
    return current_step_;
}

bool ProcessController::isReadyToNextStep() const {
    return devices_manager_.checkDevices(Situation::STANDBY);
}

bool ProcessController::isSequenceCompleted() const {
    return step_index_ >= sequence_.size();
}

void ProcessController::moveToNextStep() {
    if (step_index_ == 0 && current_step_ != "init") {
        updateDeviceStatuses(current_step_);
        return;
    }
    current_step_ = sequence_[step_index_];
    updateDeviceStatuses(current_step_);
    step_index_++;
}