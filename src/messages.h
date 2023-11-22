#pragma once

struct TLogEntry {
    int Term = 1;
    int Size = 0;
    char Data[0];
};
