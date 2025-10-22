#pragma once

#include "core/core.hpp"

class AE_Window;

struct AlineEngine
{
    AE_Window* init_create_window(u32 w, u32 h, u32 flags = 0u);
    void execute();
};
