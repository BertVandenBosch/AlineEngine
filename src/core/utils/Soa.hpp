#pragma once

// (Bert): this is on hold for now as there is currently no support for c++26
// reflection in clang #include <meta>

enum ESOA_MEMBERS_STYLE
{
    EXPLICIT, // Access TContainer::Type member values (if e.g. a custom struct)
              // by name
    ANONOYMOUS // Access TContainer::Type member values as inlined member value
               // (like jai's #using implementation)
};

template <typename TContainer, ESOA_MEMBERS_STYLE Members = EXPLICIT>
struct SOA
{

  private:
    template <typename T>
    struct proxy_generator
    {
        struct type;

        // consteval
        // {

        // };
    };
};
