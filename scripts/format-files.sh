#!/bin/sh

find examples libvalkey libvalkeycluster src tests \
    \( -name '*.c' -or -name '*.cpp' -or -name '*.h' \) \
    -exec clang-format -i {} + ;
