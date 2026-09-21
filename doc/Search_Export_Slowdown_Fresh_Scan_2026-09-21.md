# Search & Export Slowdown — Fresh Scan (2.28.1 → current branch)

**Date:** 2026-09-21
**Scope:** Independent, from-scratch review of everything that changed in `qdlt/qdltfile.cpp/.h`, `qdlt/qdltmsg.cpp`, `qdlt/qdltargument.cpp`, `qdlt/dltmessagematcher.cpp`, `qdlt/qdltexporter.cpp`, `qdlt/decodecacheservice.cpp/.h`, `src/searchdialog.cpp`, `src/searchtablemodel.cpp` from tag `2.28.1` through the current branch (`perf/tier1-tier2-fixes`, i.e. COVESA `master` plus the Tier 1/Tier 2/§3.1 fixes already applied). This document does **not** reuse conclusions from the earlier `COVESA_Release_Performance_Delta_2.28.1_2.30.0_2.31.0.md` doc or the `PR_82x` review docs — everything below was re-derived directly from `git diff`/`git log` and reading the current source.

**Reported symptoms this is investigating:**
1. `2.28.1` → `2.30.0`: search noticeably slower on large files (~500MB+).
2. `2.30.0` → current branch (master + our fixes): search **and** export noticeably slower still, on the same class of large files.

---

## Finding A — 2.30.0's original parallel Find-All did a synchronous per-message pre-pass before any parallel work started

**Introduced:** `2.28.1..2.30.0` (part of PR #760, "Search optimization and multithreaded search").
**Status on current branch:** superseded/no longer present in this exact form (see below) — relevant to symptom **#1**, not symptom **#2**.

In the `2.30.0` version of `SearchDialog::startParallelFindAll()`, before any chunk was dispatched to worker threads, the code built a filter-position snapshot like this:

```cpp
const bool useFilterSnapshot = file->isFilter();
std::shared_ptr<QVector<int>> filterPositions;
if (useFilterSnapshot)
{
    filterPositions = std::make_shared<QVector<int>>();
    filterPositions->reserve(total);
    for (int i = 0; i < total; ++i)
        filterPositions->push_back(file->getMsgFilterPos(i));   // one lock + lookup per filtered row
}
```

For a 500MB file with a large filtered result set (potentially millions of rows), this is a **synchronous, single-threaded, per-row loop on the UI thread**, executed *before* the "multithreaded" part of the search even begins — every iteration calls into `QDltFile::getMsgFilterPos()`, which takes `mutexQDlt`. This directly explains "search got slower after upgrading to 2.30.0 on a 500MB file": the very feature meant to speed up search added a new O(n) synchronous pre-pass that didn't exist in 2.28.1 at all (2.28.1 had no parallel Find-All, so no such pre-pass).

**Current state:** the current branch's `SearchProjectionSnapshot` (in `src/searchdialog.cpp`) replaced this per-row loop with a single bulk `file->getIndexFilter()` call (one lock, one implicitly-shared `QVector` copy) instead of one lock+lookup per row. So this specific regression is already gone by the time you get to master — it does **not** explain symptom #2, but it fully explains why **2.30.0 itself** felt slower than 2.28.1 for large filtered searches.

---

## Finding B — Manual markers + sort-by-time/timestamp can trigger a full disk re-read on every filter/sort/marker change

**Introduced:** `2.28.1..2.30.0` (manual-marker-always-visible feature). **Status on current branch: still present, unfixed.**

`QDltFile::mergeIndexFilterBaseWithMarkers()` ([qdlt/qdltfile.cpp](../qdlt/qdltfile.cpp)) is called from `recomputeEffectiveIndexFilterLocked()`, which itself runs after **every** filter-index rebuild, every `enableSortByTime()`/`enableSortByTimestamp()` toggle, and every `setManualMarkerIndices()` call. When there are no manual markers it short-circuits cheaply (`if(markerSet.isEmpty()) return indexFilterBase;`) — but the moment you have **any** manually marked/bookmarked message **and** sort-by-time or sort-by-timestamp enabled, it does this to merge markers into the sorted, filtered view:

```cpp
const auto keyForIndex = [&](qint64 idx) -> SortKey {
    ...
    if(idx >= 0 && idx < maxIdx && (sortByTime || sortByTimestamp))
    {
        const QByteArray data = getMsgLocked(static_cast<int>(idx));   // full message disk read
        ...
    }
    ...
};
...
while(baseIt != baseEnd && markerIt != markerEnd)
{
    if(lessByCurrentSort(*markerIt, *baseIt))      // calls keyForIndex on BOTH sides
        merged.append(*markerIt++);
    else
        merged.append(*baseIt++);
}
```

The merge walks the **base filtered list** (`indexFilterBase`, which can be the entire filtered index for a 500MB file — potentially millions of entries) one element at a time until every manual marker has been placed. Every comparison along the way calls `keyForIndex()` for the current base element, which — because sort-by-time/timestamp is on — does a **full message disk read** (`getMsgLocked()`) to get that message's timestamp. If even one manual marker sorts near the *end* of a large sorted view, this loop reads and re-parses nearly the entire filtered set from disk again, synchronously, under `mutexQDlt` — on every filter rebuild, sort toggle, or marker change.

**This did not exist in 2.28.1** (no manual-marker-always-included feature existed there), so it's a pure addition. It's conditional (needs markers + sort-by-time/timestamp together), but when triggered on a 500MB file it is severe and would plausibly be described as "everything got slower."

**Question for you:** do you (or does your typical 500MB test file / workflow) use manual markers (bookmarked messages) together with "Sort by Time" or "Sort by Timestamp"? If yes, this is very likely a primary contributor and should be prioritized.

**Suggested fix (not yet implemented):** cache each message's sort key (time/timestamp) alongside its index instead of re-deriving it via a full message read every merge, or only recompute the merge for the *changed* marker(s) rather than re-deriving sort keys for base elements that haven't changed.

---

## Finding C — Decode-required Find-All gets no multi-core speedup for the actual decode/match work (only I/O is parallel)

**Introduced:** current architecture, present since the "Single Data Model" refactor (`2.30.0..master`). **Status: ⚠️ Partially fixed** — the load/decode lockstep and one per-message copy have been removed; the core serialization (decode/match itself still runs on a single thread) remains, by design, pending a plugin thread-safety decision (see "Remaining" below).

In `SearchDialog::startParallelFindAll()` ([src/searchdialog.cpp](../src/searchdialog.cpp)), when payload search + decoder plugins are enabled (`doDecode == true` — the common case for real-world DLT files that use non-verbose/plugin-decoded payloads), the code parallelized only the **file I/O** per chunk, and did so in lockstep with decode:

```cpp
// Original: load chunk N (parallel) -> decode/match chunk N (serial) -> load chunk N+1 (parallel) -> ...
for (int begin = 0; begin < total; begin += chunkSize)
{
    ...
    const auto loadedMessages = QtConcurrent::blockingMappedReduced<...>(
        findAllPool, rows, loadMessage, appendLoaded, ...);   // parallel: raw read + QDltMsg::setMsg only

    DltMessageMatcher matcher = createMatcher();
    for (const LoadedSearchMessage &loaded : loadedMessages)   // serial: one thread, one chunk at a time
    {
        QDltMsg message = loaded.message;                      // extra copy per message
        decodeCache->decode(pluginPtr, ..., message);          // plugin decode - the expensive part
        if (matcher.match(message, searchPattern))
            matches.push_back(loaded.index);
    }
}
```

The outer `for` loop over chunks was a plain sequential loop, and the decode+match step inside it was a plain sequential `for` — **the entire decode/match phase ran on a single thread for the whole file**, chunk after chunk, with each chunk's load only starting after the previous chunk's decode finished. This is correct and necessary (plugins are documented as not thread-safe), but it meant: for any search that needs plugin decoding, you got **exactly the same single-threaded decode throughput as 2.28.1** — the "multithreaded search" only sped up the I/O/parsing portion, which is rarely the bottleneck for plugin-heavy payloads — while carrying strictly more overhead than 2.28.1's simple loop: a per-message `QDltMsg` copy, `blockingMappedReduced` scheduling overhead for the load phase happening only between decode phases instead of overlapping them, and a `decodeMutex` lock inside every `decodeMsg()` call (didn't exist in 2.28.1, needed now because decode can be called from a worker thread instead of always the UI thread).

### Fix status

**✅ Done (this session):**
1. **Removed the per-message `QDltMsg` copy.** The decode/match loop now takes `LoadedSearchMessage &` by non-const reference and decodes/matches `loaded.message` in place instead of copying it into a local `QDltMsg message` first.
2. **Pipelined load and decode across chunks.** `loadChunk(begin, end)` now returns a `QFuture` via `QtConcurrent::mappedReduced` (non-blocking) instead of `blockingMappedReduced`. The loop kicks off chunk N+1's load *before* decoding/matching chunk N, so chunk N+1's file read + parse now overlaps with chunk N's serial decode/match instead of waiting for it to finish first. Cancellation and progress-reporting granularity are unchanged.

**❌ Remaining (not done — needs a design decision, deferred per your request):**
3. **The decode/match step itself is still fully serial across the whole file.** #1 and #2 remove *added* overhead and let I/O latency hide behind decode time, but they cannot make the decode/match phase use more than one core — that requires either (a) determining that the specific decoder/viewer plugins in use are safe to call concurrently and relaxing `decodeMutex` to a per-plugin-instance lock (or no lock) for those, or (b) some other documented concurrency contract for plugins. Until that design question is resolved, decode-heavy Find-All throughput is fundamentally bounded by one core, the same as 2.28.1.

**Expected impact of what's done:** removes the copy overhead entirely (small, constant-factor win) and removes the load/decode alternation (win scales with how much of total time was I/O-bound vs. decode-bound — larger if your plugins are only moderately expensive, smaller-to-negligible if a single plugin call dominates). It does **not** change the fundamental single-core ceiling on decode-required Find-All — if your slow searches are dominated by expensive plugin decode calls, you should still expect them to take roughly as long as 2.28.1 until item 3 is addressed.

**Question for you:** when you run these slow searches, is "Payload" search enabled together with decoder/viewer plugins? If yes, item 3 above is the remaining bottleneck, and resolving it requires either determining that a subset of your plugins are actually reentrant and safe to call concurrently (relax `decodeMutex` for those), or accepting a documented single-writer decode contract and looking for gains elsewhere (there isn't much more to give on the single-thread side once items 1-2 are done).

---

## Finding D — `findMessages()` (Find Next/Previous) pays cache overhead that scales with how many distinct messages you step through, with zero benefit for a one-way scan

**Introduced:** `2.30.0..master` (Single Data Model refactor). **Status: ✅ Fixed.**

`SearchDialog::findMessages()` ([src/searchdialog.cpp](../src/searchdialog.cpp)) — used for single-step Find Next/Find Previous — used to route every candidate message through the shared decode cache:

```cpp
if(!m_decodeCacheService || !m_decodeCacheService->message(file, pluginManager, globalIndex,
                                                            decodeEnabled, fSilentMode, msg, true))
{
    continue;
}
```

`CDecodeCacheService`'s cache is capped at `kMaxEntries = 8192` ([qdlt/decodecacheservice.h](../qdlt/decodecacheservice.h)). 2.28.1's equivalent called `file->getMsgFilter(searchLine)` directly — no cache, no mutex, no hash map.

For a 500MB file, if your workflow is "type a search term, then hold/repeatedly press Find Next to scan forward through the file" (a very common pattern for large files, since you're looking for the *next* occurrence, not revisiting old ones), this was a **linear, one-way scan that never revisits a message** — the cache's hit rate was structurally ~0% once you've stepped through more than 8192 distinct messages, yet every single step still paid: `CacheKey` construction, `std::lock_guard<std::mutex>` acquisition, `unordered_map::find` (miss), then insert + FIFO push + a `pruneIfNeeded()` eviction check once the cache was full. All of that was pure overhead 2.28.1 never had, and it scaled with the number of Find-Next steps taken — which scales with file size for a thorough manual search.

A second, independent issue was found in the same code while fixing this: `decodeEnabled` was gated only by the global "plugins enabled" setting, not by whether payload search was actually checked (`getPayload()`) — unlike `startParallelFindAll()`, which correctly computes `doDecode = pluginsEnabled && payloadEnabled`. So a header/APID/CTID-only search (payload search unchecked) was still paying for a full plugin decode per candidate message, for a decoded value the matcher never uses.

**Fix applied:**
1. `decodeEnabled` is now `pluginsEnabledSetting && getPayload()` — decode is skipped entirely for header-only searches, matching `startParallelFindAll()`'s existing pattern.
2. `m_decodeCacheService->message(...)` is now called with `singlePassBypass=true`, bypassing `CDecodeCacheService`'s cache (`CacheKey`/mutex/hash map/FIFO eviction) entirely — `findMessages()` now does a direct read + (conditional) decode, the same shape as 2.28.1's `getMsgFilter()` + `decodeMsg()`. The only remaining structural differences from 2.28.1 are `QDltFile`'s own pre-existing, adaptive inner cache (harmless — it already disables itself after 32 sequential accesses) and the `decodeMutex` lock inside `decodeMsg()` (small, only matters under real cross-thread contention, and is required now that decode isn't exclusively UI-thread-only).

---

## Export — what's already fixed vs. what's still worth checking

The following export-specific overheads were already found and fixed on this branch (Tier 1/Tier 2 commits): the decode-cache single-pass bypass now being used (`singlePassBypass=true`), the double `std::vector<char>`↔`QByteArray` copy per exported message, the duplicated row-index resolution per exported line, and the discarded self-check re-parse in `createDltMessage()`. Re-reading the current, already-patched `qdlt/qdltexporter.cpp` fresh, two more observations:

- **`decodeMsg()`'s `decodeMutex`** (needed once decode became callable from multiple threads) adds a lock/unlock per exported message that 2.28.1 never paid. Uncontended, this is nanoseconds and not a real driver by itself — but if you export **while live logging is active** (decode also happening on the ingestion path), this becomes a genuine serialization point between the two, unlike 2.28.1 where decode was always UI-thread-only and could never be contended.
- No further export-specific O(n) or O(n²) issues were found beyond what's already fixed; the remaining plausible cause of "export still feels slower" is indirect — if export and search share the decode cache and you run them close together, cache eviction pressure from one can cost the other a miss it would otherwise have had (a cross-feature effect, not an export-specific bug).

**Question for you:** is the export slowdown you're seeing on its own (freshly opened file, export immediately), or does it happen after/alongside search or live logging in the same session? That distinguishes "export has its own residual cost" from "cross-feature cache/lock contention."

---

## Summary table

| # | Finding | Introduced | Fixed on current branch? | Explains symptom |
|---|---|---|---|---|
| A | Synchronous per-row `getMsgFilterPos()` pre-pass in original parallel Find-All | 2.28.1→2.30.0 | ✅ Superseded by bulk `getIndexFilter()` | #1 (2.28.1 vs 2.30.0) |
| B | Manual markers + sort-by-time/timestamp merge re-reads filtered messages from disk | 2.28.1→2.30.0 | ❌ Not fixed | #1 and #2, if you use markers+sort |
| C | Decode-required Find-All has no multi-core speedup for decode/match (I/O-only parallelism) | 2.30.0→master | ⚠️ Partially fixed — copy removed + load/decode pipelined; core serialization (item 3) still open, pending plugin thread-safety decision | #2, if payload+plugin search |
| D | `findMessages()` (Find Next/Previous) pays cache overhead with ~0% hit rate on one-way scans, and decoded unconditionally even for header-only searches | 2.30.0→master | ✅ Fixed — cache bypassed via `singlePassBypass=true`; decode now gated by `getPayload()` | #2, if using Find Next/Previous heavily |
| — | Export: decode-cache/copy overheads | 2.30.0→master | ✅ Already fixed this session | partially explains prior #2 reports |
| — | Export: `decodeMutex` contention with concurrent live logging | 2.30.0→master | Not a bug, contextual | #2, only if exporting during live logging |

## Questions before proposing fixes

1. **Do your 500MB test files/workflow use manual markers together with sort-by-time or sort-by-timestamp?** (Finding B)
2. **When search feels slow on master, is "Payload" search + a decoder/viewer plugin enabled, or is it header-only/no-plugin search?** (Finding C — this determines whether the fix needs to touch the decode pipeline's threading model, which is a bigger design change, or not)
3. **Is Find Next/Previous (single-step) your main search interaction, or do you mostly use "Find All"?** (Finding D)
4. **Is the export slowdown observed in isolation, or alongside search/live logging in the same session?**

Let me know your answers (or just say "not sure, investigate all") and I'll prioritize/implement fixes accordingly.

---

## Implementation log (this session)

Code changes applied to `src/searchdialog.cpp` (`SearchDialog::startParallelFindAll()` and `SearchDialog::findMessages()`) for Findings C and D:

1. **Removed the per-message `QDltMsg` copy** in the decode-required Find-All chunk loop — decode/match now operate directly on `loaded.message` via a non-const reference instead of copying it into a local first.
2. **Pipelined chunk loading with chunk decoding** in the same loop — `loadChunk(begin, end)` now returns a `QFuture` (`QtConcurrent::mappedReduced`, non-blocking) instead of `blockingMappedReduced`; the next chunk's file read/parse is kicked off before decoding/matching the current chunk, so I/O for chunk N+1 overlaps with decode for chunk N instead of running after it.
3. **Gated decode by payload search** in `findMessages()` — `decodeEnabled` is now `pluginsEnabledSetting && getPayload()` instead of just the global plugins-enabled setting, matching `startParallelFindAll()`'s existing `doDecode` pattern. Header/APID/CTID-only searches no longer trigger a plugin decode per candidate message.
4. **Bypassed the shared decode cache** in `findMessages()` — the call to `m_decodeCacheService->message(...)` now passes `singlePassBypass=true`, skipping the cache's `CacheKey`/mutex/hash-map/FIFO-eviction bookkeeping entirely for this scan-based access pattern.

Remaining open items from this document: Finding B (manual markers + sort-by-time/timestamp merge), Finding C item 3 (decode/match itself is still single-threaded — needs a plugin thread-safety decision), and the export `decodeMutex`-under-live-logging contention note.

