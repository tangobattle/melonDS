/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "ConsoleMemory.h"

#include <algorithm>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Args.h"
#include "NDS.h"
#include "Savestate.h"

namespace melonDS
{

NDS* ConsoleMemory::Create(NDSArgs&& args, void* userdata)
{
    // Placement-new into the reservation when there is one; a plain heap
    // console otherwise, which simply never reports any page clean.
    Reserve(sizeof(NDS));
    console = base ? new (base) NDS(std::move(args), userdata) : new NDS(std::move(args), userdata);
    return console;
}

ConsoleMemory::~ConsoleMemory()
{
    // The console is destroyed before its memory goes away, whichever
    // kind of memory that is.
    if (console)
    {
        if (base)
            console->~NDS();
        else
            delete console;
    }
#ifdef _WIN32
    if (base)
        VirtualFree(base, 0, MEM_RELEASE);
#endif
}

// Windows tracks writes in the page tables and hands the list back on
// request, which is exactly the question a snapshot has; nothing else
// here needs to know a write happened.
void ConsoleMemory::Reserve(size_t bytes)
{
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p)
    {
        base = p;
        size = bytes;
        pageGen.assign((bytes + 4095) / 4096, NEVER_CLEAN);
        written.resize((bytes + 4095) / 4096);
    }
#endif
    (void)bytes;
}

bool ConsoleMemory::DoSavestate(Savestate& state, u32 since)
{
    if (state.Error)
        return false;

    // The generation is closed here rather than by the caller so that
    // the writes this save or load itself makes land in the generation
    // after the one it reports.
    Advance();
    if (base && since != 0)
        state.SetDirtyPages(base, (u32)size, pageGen.data(), since);

    // The first full save is where the ranges to watch come from; it
    // moves every byte anyway, so recording costs it nothing. A load
    // never learns: it is not the console writing, and the generation
    // its writes belong to has not been closed yet.
    const bool learning = base && watched.empty() && state.Saving;
    if (learning)
    {
        bulk.clear();
        state.RecordBulkArrays(&bulk);
    }

    const bool ok = console->DoSavestate(&state) && !state.Error;

    if (learning)
    {
        state.RecordBulkArrays(nullptr);
        // A state that failed part way moved only part of what it
        // reported, so there is nothing here to call clean.
        if (ok)
            Learn();
    }
    return ok;
}

void ConsoleMemory::Advance()
{
    if (!base)
        return;
#ifdef _WIN32
    generation++;
    ULONG granularity = 0;

    // Ask only about the ranges the bulk copies read: the reservation is
    // a whole console and most of it — timing tables, framebuffers, the
    // renderer's scratch — is never serialized, but the kernel still
    // walks the page tables of whatever it is asked about, which costs
    // more than the copy the answer saves.
    const bool learned = !watched.empty();
    const size_t queries = learned ? watched.size() : 1;
    for (size_t q = 0; q < queries; q++)
    {
        u8* from = (u8*)base;
        size_t bytes = size;
        if (learned)
        {
            from = (u8*)base + (watched[q].first << 12);
            bytes = (watched[q].second - watched[q].first) << 12;
        }
        ULONG_PTR count = written.size();
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, from, bytes, written.data(), &count, &granularity) != 0)
        {
            // The kernel refused: treat the range as written, which
            // costs a full copy and stays correct.
            const size_t first = (from - (u8*)base) >> 12;
            std::fill(pageGen.begin() + first, pageGen.begin() + first + (bytes >> 12), generation);
            continue;
        }
        for (ULONG_PTR i = 0; i < count; i++)
        {
            size_t page = ((u8*)written[i] - (u8*)base) >> 12;
            if (page < pageGen.size())
                pageGen[page] = generation;
        }
    }
#endif
}

// The arrays' pages start clean as of the generation the save that
// reported them carries — it moved every one of them — and every other
// page keeps NEVER_CLEAN, so nothing outside is ever skipped.
void ConsoleMemory::Learn()
{
    if (!base || !watched.empty() || bulk.empty())
        return;

    std::vector<std::pair<size_t, size_t>> pages;
    for (auto& entry : bulk)
    {
        const u8* p = (const u8*)entry.first;
        if (p < (u8*)base || p + entry.second > (u8*)base + size)
            continue; // outside the reservation: never eligible anyway
        pages.emplace_back((size_t)(p - (u8*)base) >> 12,
                           ((size_t)(p + entry.second - 1 - (u8*)base) >> 12) + 1);
    }
    if (pages.empty())
        return;

    std::sort(pages.begin(), pages.end());
    for (auto& range : pages)
    {
        if (!watched.empty() && range.first <= watched.back().second)
            watched.back().second = std::max(watched.back().second, range.second);
        else
            watched.push_back(range);
    }

    // Put back every sentinel the whole-reservation pass overwrote.
    // Until the ranges were known that pass stamped a generation onto
    // every page it found written, unwatched ones included — and an
    // unwatched page is never asked about again, so it would have kept
    // that generation and read as clean forever after.
    std::fill(pageGen.begin(), pageGen.end(), NEVER_CLEAN);
    for (auto& range : watched)
        std::fill(pageGen.begin() + range.first, pageGen.begin() + range.second, generation);
}

}
