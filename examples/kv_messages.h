#pragma once
#include <miniraft/messages.h>

struct TKvEntry {
    uint16_t KeySize = 0;
    uint16_t ValSize = 0;
    char Data[0];
};

struct TKvLogEntry: public TLogEntry, public TKvEntry
{
};

struct TWriteKv: public TCommandRequest, public TKvEntry
{
};

struct TReadKv: public TCommandRequest {
    uint16_t KeySize = 0;
    char Data[0];
};

