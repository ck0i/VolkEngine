#include "platform/Window.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<ve::GlfwRuntime>);
static_assert(!std::is_copy_assignable_v<ve::GlfwRuntime>);
static_assert(!std::is_move_constructible_v<ve::GlfwRuntime>);
static_assert(!std::is_move_assignable_v<ve::GlfwRuntime>);
static_assert(std::is_constructible_v<ve::Window, ve::GlfwRuntime&, const ve::EngineConfig&>);
static_assert(!std::is_constructible_v<ve::Window, const ve::EngineConfig&>);
static_assert(!std::is_copy_constructible_v<ve::Window>);
static_assert(!std::is_move_constructible_v<ve::Window>);
static_assert(!std::is_move_assignable_v<ve::Window>);
static_assert(!std::is_copy_assignable_v<ve::Window>);

int main() {
    return 0;
}
