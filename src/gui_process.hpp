#pragma once

#include <memory>
#include <string>
#include <vector>

namespace udm {

void gui_askpass();

class GuiCommand {
public:
    struct State;

    GuiCommand();
    ~GuiCommand();

    GuiCommand(const GuiCommand&) = delete;
    GuiCommand& operator=(const GuiCommand&) = delete;
    GuiCommand(GuiCommand&&) = delete;
    GuiCommand& operator=(GuiCommand&&) = delete;

    void start(std::vector<std::string> argv, std::string password);
    void poll();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] std::string output() const;
    [[nodiscard]] int exit_code() const;

private:
    std::shared_ptr<State> state_;
};

}
