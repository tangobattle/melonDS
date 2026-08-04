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

#ifndef CONSOLEMEMORY_H
#define CONSOLEMEMORY_H

#include <cstddef>
#include <utility>
#include <vector>

#include "types.h"

namespace melonDS
{
class NDS;
struct NDSArgs;
class Savestate;

// A console living in memory the operating system watches, so that a
// savestate can ask which of its pages the frames since the last one
// actually touched — the hardware's own dirty bits, at no cost to the
// emulation. A rollback session snapshots every tick and a battle moves
// ~3% of a console's state per tick, so copying only what moved is most
// of a snapshot's cost. `Savestate::SetDirtyPages` is the other half of
// this; here is where the record it reads comes from.
//
// Where the memory cannot be write-watched (any platform but Windows)
// the console goes on the heap instead and every savestate moves
// everything, exactly as it would without any of this.
class ConsoleMemory
{
public:
    ConsoleMemory() = default;
    ~ConsoleMemory();
    ConsoleMemory(const ConsoleMemory&) = delete;
    ConsoleMemory& operator=(const ConsoleMemory&) = delete;

    // Build the console this memory is for — once, before anything
    // else. Never null; the console is destroyed with this object.
    NDS* Create(NDSArgs&& args, void* userdata);
    [[nodiscard]] NDS* Console() const noexcept { return console; }

    // The generation the record now holds: the tag a buffer filled by
    // the save that reported it carries, and hands back as `since` the
    // next time round. Zero where no pages are tracked, which is what
    // disables the shortcut for a console that has no record.
    [[nodiscard]] u32 Generation() const noexcept { return base ? generation : 0; }

    // Run `state` over the console, moving only the pages written since
    // generation `since` — 0 moves everything, which is the only safe
    // answer for a buffer this console did not fill itself. Returns
    // false if the state failed, which for a save means the buffer was
    // too small.
    bool DoSavestate(Savestate& state, u32 since);

private:
    // Reserve `bytes` of write-watched memory, or leave `base` null
    // where the operating system has no such thing.
    void Reserve(size_t bytes);
    // Close the current generation: every page written since the last
    // call is stamped with it. A buffer filled right after this carries
    // the generation as its own, and a page written later gets a higher
    // one — which is the whole test a save or a load then makes.
    void Advance();
    // Adopt the bulk arrays a full save just reported as the ranges
    // worth watching.
    void Learn();

    NDS* console = nullptr;
    void* base = nullptr;
    size_t size = 0;

    // Per page, the generation at the end of which it was last written,
    // indexed by (address - base) >> 12. A page outside every watched
    // range keeps NEVER_CLEAN, so nothing is ever skipped on the word of
    // a record that is not being kept.
    static constexpr u32 NEVER_CLEAN = 0xFFFFFFFF;
    std::vector<u32> pageGen;
    u32 generation = 0;

    // The page ranges worth asking the kernel about: the bulk arrays a
    // savestate actually moves, learned from the first one. Empty until
    // then, which means "the whole reservation".
    std::vector<std::pair<size_t, size_t>> watched;

    // GetWriteWatch's output buffer, kept rather than reallocated: it is
    // one page pointer per dirty page and this runs every tick.
    std::vector<void*> written;

    // Where a save reports its bulk arrays, kept for the same reason.
    std::vector<std::pair<const void*, u32>> bulk;
};

}

#endif // CONSOLEMEMORY_H
