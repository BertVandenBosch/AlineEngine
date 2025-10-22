#pragma once

#include "core/core.hpp"

class AE_Window;

struct AlineEngine
{
    AE_Window* init_create_window(uint32 w, uint32 h, uint32 flags = 0u);
    void execute();
};