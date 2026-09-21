# COVESA dlt-viewer — Runtime Performance Delta: 2.28.1 → 2.30.0 → 2.31.0 (master)

**Repo analyzed:** `git@github.com:COVESA/dlt-viewer.git` (remote `COVESA` in this workspace)
**Refs compared:**

| Label                                 | Ref               | Commit       | Date       |
| ------------------------------------- | ----------------- | ------------ | ---------- |
| 2.28.1                                | tag`2.28.1`     | `fe1bbda9` | 2025-07-30 |
| 2.30.0                                | tag`2.30.0`     | `60afcfad` | 2026-03-13 |
| 2.31.0 (unreleased, latest`master`) | `COVESA/master` | `f2e0e22b` | 2026-09-11 |

There is no `2.31.0` tag yet — `master` is one commit ("PR Review fix") past the last "Phase 7" merge and is treated here as the 2.31.0 candidate. This workspace's checked-out `HEAD` (`83e15281`, branch `pr-840`) has an **identical tree** to `COVESA/master`, so all file/line references below are verifiable directly in the current working copy.

Commit counts: 189 commits between `2.28.1..2.30.0`, 63 commits between `2.30.0..master`.

This document only covers **runtime performance** relevant changes (allocation, locking, caching, algorithmic complexity, thread usage, I/O), not features/UI/CI-only changes. Functional/crash bugs are mentioned only where they are the direct side effect of a performance-motivated change.

---

## Negative-Impact Findings — Quick Reference

All findings below are confirmed present on current `master` (2.31.0 candidate). "Fix size" is an estimate of code-change effort, not calendar time. "Fixed in branch?" reflects the `perf/tier1-tier2-fixes` branch (commits `fc4429be`, `c0ffa125`, `817fe6eb`) as of 2026-09-21.

| # | Severity | Impacting feature / area | Fix size (code change needed) | Finding | Fixed in branch? |
|---|---|---|---|---|---|
| 1 | High | File open / indexing / CLI export (`commander`) — load-time I/O doubling on every file open | **Medium** — restructure `calculateTotalSizes()` logic into the existing single-pass `updateIndex()` loop in `qdlt/qdltfile.cpp` (one file, one function reorganized, no API/signature change) | [§3.1](#31-createindex-does-a-redundant-full-second-file-scan-on-every-open-high--load-time-regression--fixed) Redundant full second file scan in `createIndex()` | ✅ Done — size accumulation folded directly into `updateIndex()`'s single scanning pass; `createIndex()` no longer calls `calculateTotalSizes()` at all |
| 2 | High | CRLF filter window / ECU-grouped views under concurrent live logging or filter rebuild — data race / intermittent crash | **Small** — swap one call site from `getIndexFilterRef()` to the existing locked copy accessor (`getIndexFilter()`), or wrap the read in `QMutexLocker`; single function, no design change | [§3.2](#32-unsynchronized-read-of-the-live-filter-index-crit-01-data-race--high) Unsynchronized read of the live filter index | ✅ Done — `Tier 1` (`fc4429be`); all 3 call sites switched to `getIndexFilter()`, unsafe `getIndexFilterRef()` accessor removed entirely |
| 3 | High | Search dialog / search results table — null-pointer crash if decode cache service isn't wired up | **Trivial** — add `if (m_decodeCacheService && ...)` guards at ~3 call sites; no logic/design change | [§3.3](#33-unguarded-null-m_decodecacheservice-on-the-rendersearch-hot-path-crit-02--high-crash-not-perf-but-on-the-same-hot-path-as-22) Unguarded null `m_decodeCacheService` | ✅ Done — `Tier 1` (`fc4429be`) |
| 4 | High | Search dialog "Find All" (parallel path) — steady memory/handle leak with repeated use | **Small** — replace the per-call `QThreadStorage<QFile*>` with a plain per-chunk `QFile` (or a pool-lifetime-scoped storage with explicit cleanup); localized to `startParallelFindAll()` | [§3.4](#34-qthreadstorageqfile-leak-in-parallel-find-all-high--steady-memory-growth) `QThreadStorage<QFile*>` leak in parallel Find-All | ✅ Done — `Tier 1` (`fc4429be`); switched to a single pool-lifetime-scoped `QThreadStorage` shared across all Find-All runs |
| 5 | Medium | "Group DLT Logs by ECU" feature — UI freeze/"Not Responding" on large files | **Medium/Large** — move `extractEcuIds()` scan to a background thread with progress reporting (`QtConcurrent`/`QFutureWatcher`, mirroring the existing Find-All pattern) and delete the dummy progress loop in `mainwindow.cpp`; touches 2 files and changes control flow | [§3.5](#35-group-dlt-logs-by-ecu--synchronous-full-file-scan--fake-progress-bar-on-the-ui-thread-perf-01perf-02--medium) Synchronous ECU-ID scan + fake progress bar | ❌ Not done |
| 6 | Low/Medium | Search / Find-All time-range filtering — boundary messages silently excluded; also confounds perf-regression test result counts | **Trivial** — change two comparison operators (`<` → `<=`) in `matchTimeRangeMs()` | [§3.6](#36-time-range-search-excludes-exact-boundary-timestamps-logic-01--lowmedium-correctness-not-perf-but-affects-test-validity) Time-range search excludes exact boundary timestamps | ✅ Done — `Tier 1` (`fc4429be`) |
| 7 | Low | CRLF/ECU-grouped projection views during high-rate live logging — extra CPU per incoming message | **Medium** — add a sorted/hash index alongside `m_projectionRows` for O(log n) lookup and/or coalesce `dataChanged` bursts; localized to `projectiontablemodel.cpp` but needs care to keep signal semantics correct | [§3.7](#37-projectiontablemodelonsourcedatachanged-does-an-orows-linear-scan-per-source-change-low) O(rows) linear scan per `dataChanged` | ✅ Done — `Tier 2` (`c0ffa125`); replaced linear scan with binary search over the ascending-ordered `m_projectionRows` |
| 8 | High | **Export** (File Export dialog, all formats) — every exported message pays a mutex-protected cache lookup/insert with zero hit-rate benefit | **Small** — pass the already-supported `singlePassBypass=true` argument to `decodeCacheService.message(...)` at the one call site in `qdlt/qdltexporter.cpp`; no new code, just use an existing parameter | [§3.8](#38-export-routes-every-message-through-the-shared-mutex-protected-decode-cache-instead-of-the-single-pass-bypass-high) Export bypasses the single-pass cache-bypass it already has access to | ✅ Done — `Tier 1` (`fc4429be`) |
| 9 | Medium | **Export** (all formats) — two extra full-buffer copies per exported message vs. 2.28.1 | **Medium** — add a direct `QByteArray`-returning accessor on `CQDltFileMessageStoreAdapter`/`CMessageStore` (or have `QDltExporter::getMsg` call `QDltFile` directly for the raw-bytes case) instead of round-tripping through `std::vector<char>`; touches `messagestore.h/.cpp` + `qdltexporter.cpp` | [§3.9](#39-export-copies-message-bytes-twice-via-messagestore-indirection-medium) Double buffer copy per exported message | ✅ Done — `Tier 2` (`c0ffa125`); added `rawMessageBytes()` returning `QByteArray` directly |
| 10 | Low | **Export** (Text/CSV/Clipboard formats) — row-index resolved twice per exported line | **Trivial** — compute `globalIndexForSelectionRow(num)` once in `exportMsg()`/`getMsg()` and pass/reuse the value instead of recomputing | [§3.10](#310-redundant-row-index-resolution-computed-twice-per-exported-row-low) Redundant index resolution per row | ✅ Done — `Tier 1` (`fc4429be`); reuses `msg.getIndex()` instead of recomputing |
| 11 | Low | **Export** → "Decoded DLT" format for non-verbose messages — extra full re-parse per message purely for a discarded self-check | **Trivial** — delete the `checkMsg.setMsg(out, true, true)` self-check block in `createDltMessage()` (or gate it behind a debug-only build flag) | [§3.11](#311-formatdltdecoded-self-check-re-parse-added-per-exported-non-verbose-message-low) Discarded self-check re-parse in `createDltMessage()` | ✅ Done — `Tier 1` (`fc4429be`) |
| 12 | Medium | **Search** — interactive Find Next/Previous and every Find-All candidate now pay decode-cache mutex+hash overhead that 2.28.1 didn't have, worst-case thrashing once a search touches more than the cache's 512-entry cap | **Medium** — add a `singlePassBypass`-style fast path for Find-All (a single forward scan never needs caching), and/or raise/parameterize the cache size for search use; touches `searchdialog.cpp` call sites and possibly `decodecacheservice.h` capacity constant | [§3.12](#312-search-now-pays-decode-cache-mutexhash-overhead-2281-never-had-medium) Search decode-cache overhead vs. 2.28.1's direct file access | ✅ Verified, no change needed — investigated in `Tier 2`; the decode-required Find-All path already calls `CDecodeCacheService::decode()` directly (not `message()`), which never touches the shared cache/mutex. `findMessages()` (Find Next/Previous) intentionally still uses the cache since it legitimately benefits from revisits |
| 13 | High | **Main table view** (scrolling/rendering) — the single most-used code path in the app; every visible cell now pays the shared cache's mutex+hash lookup once per requested role instead of once per row | **Small** — reintroduce a one-entry "last row" message cache inside `CTableModel` checked before falling through to `m_decodeCacheService`; localized to `tablemodel.cpp`, no design change | [§3.13](#313-main-table-lost-its-cheap-one-entry-row-cache-in-favor-of-the-shared-mutexprotected-cache-high) Main table lost its cheap one-entry row cache | ✅ Done — `Tier 1` (`fc4429be`) |
| 14 | Medium | **All decode-using features simultaneously** (live logging, table render, search, export, CRLF, ECU grouping) — one global mutex now serializes decode calls across every feature, a contention point that didn't exist in 2.28.1 (decode was UI-thread-only, no lock needed) | **Medium/Large** — requires a concurrency-model decision (per-plugin-instance locking, thread-confined plugin pools, or documented/enforced single-writer contract), not a local one-line fix; touches `qdltpluginmanager.cpp`/`.h` and every call site's assumptions | [§3.14](#314-global-plugin-decode-mutex-serializes-every-decode-using-feature-medium) Global plugin-decode mutex cross-feature contention | ❌ Not done — needs a design decision first |
| 15 | Medium | **CRLF Filter window** — same synchronous full-scan-on-UI-thread pattern as ECU grouping (§3.5), reentrancy crash already mitigated but the UI-thread stall on cold cache remains | **Medium** — move `buildCrlfProjectionRows()`'s scan to a background thread with a `QFutureWatcher`, mirroring the Find-All pattern; touches `crlffilterwindow.cpp` | [§3.15](#315-crlf-filter-window-shares-ecu-groupings-synchronous-ui-thread-scan-pattern-medium) CRLF window's synchronous UI-thread scan | ✅ Done — `Tier 2` (`c0ffa125`); scan moved to `QtConcurrent::run`, UI thread only pumps progress/cancel via a `QFutureWatcher`-driven event loop |

---

## 0. Direct answer: is Search / Export faster or slower on master vs. 2.28.1?

**Export: net regression, no offsetting win found.** Every code path that touched export between 2.28.1 and master (`a216b87b`'s indexing-stats work, then the Phase 1–7 MessageStore/DecodeCacheService refactor) added pure overhead with no new export-specific optimization to offset it: an unused single-pass cache-bypass ([§3.8](#38-export-routes-every-message-through-the-shared-mutex-protected-decode-cache-instead-of-the-single-pass-bypass-high)), two extra buffer copies per message ([§3.9](#39-export-copies-message-bytes-twice-via-messagestore-indirection-medium)), a duplicated per-row index lookup ([§3.10](#310-redundant-row-index-resolution-computed-twice-per-exported-row-low)), a discarded self-check re-parse for decoded non-verbose export ([§3.11](#311-formatdltdecoded-self-check-re-parse-added-per-exported-non-verbose-message-low)), and — separately, on the load side — a redundant full second file scan that also runs before every `commander`/ECU-grouping export ([§3.1](#31-createindex-does-a-redundant-full-second-file-scan-on-every-open-high--load-time-regression)). All five are additive per-message or per-load costs; none replace an equally expensive 2.28.1 operation. This is consistent with a perceived export slowdown, and all five are cheap, localized fixes (§3.8 is a one-line change).

**Search: mixed — Find-All is faster or equal, but the newer per-message paths pay overhead 2.28.1 didn't have.** The multithreaded Find-All work (`277d4644`/`3a49c679`, landed before 2.30.0, kept and extended through Phase 7) is a genuine, real win versus 2.28.1's fully serial search — 2.28.1 had **no parallelism at all** for Find-All, so master's non-decode parallel path is strictly better, and the decode-required path is no worse (both are effectively serial due to plugin thread-safety limits). Where master can regress: (a) every message compared in `findMessages()` (Find Next/Previous) and in decode-required Find-All now goes through the shared, mutex-protected, 512-entry `CDecodeCacheService` instead of 2.28.1's direct, lock-light `file->getMsgFilter()` call — pure overhead for the (common) case where a linear scan never revisits a message ([§3.12](#312-search-now-pays-decode-cache-mutexhash-overhead-2281-never-had-medium)); (b) that same shared cache is also used by the main table/CRLF/ECU views, so a large search run can evict cache entries those views depend on and vice versa, making other UI paths feel slower right after/during a big search. Net: if what you're perceiving as "slower search" is Find-All on a large file, it should not be slower than 2.28.1 by design — worth confirming with a timed A/B run (§3.12's test steps) before assuming a regression there; if it's Find Next/Previous on a file with a very large working set, §3.12 is the concrete, confirmed cause.

---

## 1. How this was produced

- `git fetch COVESA --tags` to get up-to-date refs, then `git log --oneline <range>` for both ranges.
- Cross-referenced with this fork's own prior PR review documents in [doc/](.) ([PR_822_Code_Review.md](../PR_822_Code_Review.md), [PR_823_phase5_append_fix_review_2026-08-12.md](PR_823_phase5_append_fix_review_2026-08-12.md), [PR_825_phase3_search_delink_review_2026-08-21.md](PR_825_phase3_search_delink_review_2026-08-21.md), [PR_826_phase2_live_log_delink_review_2026-09-08.md](PR_826_phase2_live_log_delink_review_2026-09-08.md), [PR_827_phase4_render_cache_delink_review_2026-08-20.md](PR_827_phase4_render_cache_delink_review_2026-08-20.md), [PR_840_code_review_2026-09-14.md](PR_840_code_review_2026-09-14.md)) — these were written against **intermediate** branch states, some of which were later reverted/replaced. Every finding reused here was **independently re-verified against the current `master` tree** with `grep`/`read_file`, since several of the earlier documented issues (e.g. `drawTimer` reuse, the non-recursive mutex re-entry deadlock) were already fixed in later commits and the whole async `IndexThreadWorker`/`FilterThreadWorker` subsystem those docs describe was removed again before merge.
- No benchmark harness currently exists in the repo for these paths (no Google Benchmark target). Validation suggestions below use the existing manual/functional reproduction style already used in this repo's review docs, since that is the narrowest available validation method.

---

## 2. Positive-impact performance changes

### 2.1 Between 2.28.1 and 2.30.0

| Change                                                                    | Commit                                 | Effect                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ------------------------------------------------------------------------- | -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `toStringHeader`/`toStringPayload` caching                            | `70f3cab4`                           | [qdlt/qdltmsg.cpp](../qdlt/qdltmsg.cpp) adds a `thread_local QCache<HeaderSuffixKey, QString>` for the invariant part of the header string, plus a `thread_local` last-second cache for the formatted timestamp (`cachedTimeString`). Avoids repeated `QString` concatenation/allocation and repeated `localtime`/`strftime` calls for consecutive messages sharing the same second — a real win on the hot render/export path. |
| Search optimization + multithreaded search                                | `277d4644`, `3a49c679`             | Introduces`QtConcurrent`-based Find-All and short-circuiting in `DltMessageMatcher` (header mismatch returns immediately instead of falling through).                                                                                                                                                                                                                                                                                   |
| Live logging performance improvement                                      | `4ecfb0ca`                           | [qdlt/qdltfile.cpp](../qdlt/qdltfile.cpp): skips marker-index recompute entirely when `manualMarkerIndices.isEmpty()`, avoiding wasted work on the common no-markers live-logging path.                                                                                                                                                                                                                                                    |
| Skip CFI (create-filter-index) pass when no filter is active              | part of the search-optimization series | Avoids a full linear filter pass during live logging when the user has no filter enabled.                                                                                                                                                                                                                                                                                                                                                   |
| Revert of "start filter index after adding at least one filter condition" | `38da3b3f`                           | An earlier attempt to lazily defer filter-index construction was reverted — it changed observable behavior/timing without a clear net win; reverting avoided shipping a regression.                                                                                                                                                                                                                                                        |

### 2.2 Between 2.30.0 and 2.31.0 (master) — "Single Data Model" refactor (Phases 1–7)

This is the dominant change in this range: a 7-phase architectural rework (`MessageStore` → `IndexService`/`DecodeCacheService` → incremental `CTableModel` updates → `ProjectionTableModel` for derived views → plugin contract hardening → cache-bypass/fast-matcher/parallel search), landed, **fully reverted**, then **re-landed** with fixes, followed by two more hardening commits ("Improve filtering and search handling", "Update filter marker checks and refresh logic") and a final "PR Review fix". Net effect on `master` today:

| Change                                                                                                                                                     | Where                                                                                      | Effect                                                                                                                                                                                                                                                                                                           |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CDecodeCacheService` — shared decode cache with FIFO eviction, keyed by pipeline generation                                                            | [qdlt/decodecacheservice.h](../qdlt/decodecacheservice.h)/`.cpp`                          | Search, export, CRLF/ECU-grouping and the table model now share one decoded-message cache instead of each feature independently calling`pluginManager->decodeMsg()`, cutting redundant plugin-decode work for the same message across features used together (e.g. searching while the main table is visible). |
| `QDltFile` cache admission heuristics: `shouldUseMessageCache`, `shouldAdmitCacheInsertLocked`, `cacheSinglePassBypass`, sequential-scan detection | [qdlt/qdltfile.h](../qdlt/qdltfile.h) (~L419-480), [qdlt/qdltfile.cpp](../qdlt/qdltfile.cpp) | Bulk, one-pass scans (indexing, marker counting, size calculation) now bypass the`QCache` entirely instead of thrashing it with one-shot inserts that would never be reused — avoids allocation/eviction churn during indexing.                                                                               |
| `DltMessageMatcher` caches its UTF-8 pattern once and early-exits on an empty string pattern                                                             | [qdlt/dltmessagematcher.cpp](../qdlt/dltmessagematcher.cpp)                                 | Removes repeated`QString→UTF-8` conversion per message during Find-All.                                                                                                                                                                                                                                       |
| Parallel Find-All via`QThreadPool` + `SearchProjectionSnapshot` (immutable, implicitly-shared)                                                         | [src/searchdialog.cpp](../src/searchdialog.cpp) (`startParallelFindAll`, ~L236-330)       | When no plugin decode is required, the file is chunked (≤20000 rows/chunk) and matched in parallel across a thread pool sized to`maxThreadCount()`, instead of one linear UI-thread pass.                                                                                                                     |
| `ProjectionTableModel` for CRLF/ECU-grouped views                                                                                                        | [src/projectiontablemodel.cpp](../src/projectiontablemodel.cpp)                             | Derived views reference rows in the main model instead of duplicating message data into a separate`QStandardItemModel`, cutting memory copy cost for large filtered subsets.                                                                                                                                   |

**Net verdict:** the direction is sound and several of these are measurable wins for search and rendering. However, the same refactor introduced the negative-impact issues below, several of which are **still present on current `master`** and were not resolved by the later "Improve filtering and search handling" / "PR Review fix" commits.

---

## 3. Confirmed negative-impact / risk findings on current `master` (2.31.0 candidate)

Each item below was re-checked directly against the current tree (not just the historical PR docs) with a `grep`/`read_file` on 2026-09-17.

### 3.1 `createIndex()` does a redundant full second file scan on every open (High — load-time regression) — ✅ Fixed

**Where:** [qdlt/qdltfile.cpp](../qdlt/qdltfile.cpp) `QDltFile::createIndex()` (~L337-352) unconditionally called `calculateTotalSizes()` after `updateIndex()`; `calculateTotalSizes()` re-read **every single message** a second time, purely to parse storage/DLT headers for size accounting (`totalStorageSize`/`totalMessageSize`/`totalPayloadSize`).

**Callers:** `commander/main.cpp` (CLI batch export/filter tool) and `src/filtergrouplogs.cpp::extractEcuIds()` fallback path (opens a **second, throwaway** `QDltFile` and re-indexes+re-scans the whole file from disk again, in addition to the file already loaded in the main window).

**Why this was a regression:** introduced in `a216b87b` ("Generating statistics of DLT file during the indexing") between 2.28.1 and 2.30.0. For a multi-GB / multi-million-message log, this doubled disk I/O and full-file parsing time at load/export time.

**Fix applied:** `updateIndex()`'s single marker-scanning pass already determines each message's total byte size, DLT length field, and header-type byte while locating it — those values are now captured (`current_htyp`, `dltMessageLengthOnly`) and fed directly into a new `accumulateMessageSizeLocked()` helper at both message-boundary points in the scan, updating `totalStorageSize`/`totalMessageSize`/`totalPayloadSize` incrementally with **zero extra I/O**. `createIndex()` no longer calls `calculateTotalSizes()` at all. `calculateTotalSizes()` itself (with its Tier-2 bounded-prefix-read optimization) remains only as a lazy fallback inside `getTotalStorageSize()`/etc. for the edge case where sizes are queried before any scan has populated them.

**How to test / reproduce (verify the fix):**

1. Load a large DLT file (≥1M messages / several GB) and confirm via a profiler or I/O monitor (Resource Monitor / `iostat`) that the file is read only once during open, not twice.
2. Compare `getTotalStorageSize()`/`getTotalMessageSize()`/`getTotalPayloadSize()` values against the previous (pre-fix) implementation on the same file to confirm the incrementally-accumulated totals still match.
3. For "Group DLT Logs by ECU": load a large file, click the feature, and confirm the fallback path (`extractEcuIds()` → `createIndex()`) no longer performs a double read either.

**Remaining, lower-priority follow-up:** `extractEcuIds()`'s fallback still opens a second, throwaway `QDltFile` instance when the shared `messageStore`/`decodeCacheService` aren't wired up — that duplicate *file open*, while now only a single-pass scan instead of a double one, is still avoidable by preferring the shared-service path (see §3.5).

### 3.2 Unsynchronized read of the live filter index (`CRIT-01`, data race) — High

**Where:** [qdlt/qdltfileprojection.cpp](../qdlt/qdltfileprojection.cpp) L40, `buildActiveFilteredProjection()`:

```cpp
const auto &filterRef = file->getIndexFilterRef(); // reference, no lock held while iterating
```

`getIndexFilterRef()` returns a direct reference to `QDltFile::indexFilter`, which background indexing/filter-recompute code mutates under `mutexQDlt`. Iterating this reference without holding the lock races with any concurrent `recomputeEffectiveIndexFilterLocked()` (e.g. during live logging or a filter-index rebuild), risking a crash from concurrent `QVector` reallocation.

**Impacted features:** CRLF filter window, ECU-grouped views, anything else that calls `buildActiveFilteredProjection()` while live logging or re-filtering is in progress.

**How to test:**

1. Open a large file, start live logging or trigger a filter change/rebuild in a loop (script it via the settings/filter dialog or a debug hook).
2. Concurrently open the CRLF Filter window / ECU grouping window repeatedly while the above is running.
3. Run under Application Verifier / Visual Studio's Concurrency Sanitizer equivalent, or repeat rapidly enough (hundreds of iterations) to raise the odds of an interleaving that hits mid-reallocation; look for intermittent `SIGSEGV`/heap-corruption crashes.

**Suggested fix:** use the existing thread-safe accessor that returns a copy under `mutexQDlt` (e.g. `file->getIndexFilter()`) instead of `getIndexFilterRef()`, or take the lock for the duration of the copy in `buildActiveFilteredProjection()`.

### 3.3 Unguarded null `m_decodeCacheService` on the render/search hot path (`CRIT-02`) — High (crash, not perf, but on the same hot path as §2.2)

**Where:** [src/searchtablemodel.cpp](../src/searchtablemodel.cpp) L58/208/236 and [src/searchdialog.cpp](../src/searchdialog.cpp) call `m_decodeCacheService->message(...)` with no null check; `m_decodeCacheService` defaults to `nullptr` until explicitly injected.

**How to test:** construct `CSearchTableModel`/`CSearchDialog` without calling the cache-service setter (e.g. from a unit test, or any future code path that forgets to wire it up) and call `data()`/`findMessages()` — immediate null-pointer crash.

**Suggested fix:** guard every call site with `if (m_decodeCacheService && m_decodeCacheService->message(...))`, matching the pattern already used elsewhere in the same file (e.g. `invalidateDecodeCache()`, L344-345).

### 3.4 `QThreadStorage<QFile*>` leak in parallel Find-All (High — steady memory growth)

**Where:** [src/searchdialog.cpp](../src/searchdialog.cpp) L411-430, `startParallelFindAll()`:

```cpp
QThreadStorage<QFile*> workerReaders;              // local to this call
auto loadMessage = [=, &workerReaders](int row) {
    QFile *workerReader = workerReaders.localData();
    if (!workerReader) {
        workerReader = new QFile;                  // heap-allocated
        workerReaders.setLocalData(workerReader);
    }
    ...
};
```

`workerReaders` is a **local** `QThreadStorage`. `QThreadStorage` only frees its per-thread payload when the *owning thread* terminates, not when the `QThreadStorage` object itself is destroyed. Because Find-All runs on a shared, reused `QThreadPool`, worker threads outlive this call — every "Find All" run that reuses an existing pool thread leaks one `QFile*` (the previous run's local-data pointer is simply overwritten/orphaned when a new `QThreadStorage` instance is created on the next call).

**How to test:**

1. Load any DLT file, open Search, run "Find All" (non-decode path, so it takes the parallel branch) 20-30 times in a row.
2. Track private-bytes/heap usage in Task Manager, `VMMap`, or Visual Studio's Diagnostic Tools "Memory Usage" snapshot between runs.
3. Expect a small but steady per-run increase proportional to the thread-pool size (not the file size), confirming leaked `QFile` objects rather than data-dependent growth.

**Suggested fix:** don't rely on `QThreadStorage` for a value whose lifetime should be scoped to the single Find-All call. Either allocate a plain (non-thread-local) `QFile` per invocation of the lambda/chunk (cheap relative to chunk size), or use a pool-lifetime-scoped `QThreadStorage` owned by `SearchThreadPool`/`findAllThreadPool()` itself (constructed once, reused, with an explicit cleanup function) instead of one created fresh per search.

### 3.5 "Group DLT Logs by ECU" — synchronous full-file scan + fake progress bar on the UI thread (`PERF-01`/`PERF-02`) — Medium

**Where:**

- [src/filtergrouplogs.cpp](../src/filtergrouplogs.cpp) `extractEcuIds()` (~L37-58): iterates `messageStore->snapshotAllMessageIds()` and decodes every message synchronously on the calling (UI) thread before the "Grouping DLT Logs by ECU ID..." progress dialog is even shown.
- [src/mainwindow.cpp](../src/mainwindow.cpp) ~L7137-7151: the progress dialog's `for (int i = 0; i < rowCount; ++i) { progress.setValue(i+1); QCoreApplication::processEvents(); ... }` loop does **no actual work** — it's a fixed-count busy loop that runs to 100% before `filterLogsEcuid->ecuIdTabs()` (the real work) even starts.

**Impact:** on a multi-million-message file, `extractEcuIds()` blocks the UI thread for the entire scan (application shows "Not Responding"), and the subsequent fake progress loop burns additional CPU cycles/`processEvents()` churn giving a false sense that the work already happened.

**How to test:**

1. Load a DLT file with several million messages.
2. Click "Group DLT Logs by ECU" and watch for the OS-level "(Not Responding)" title-bar state during `extractEcuIds()`, then observe the progress bar filling 0→100% before the real grouping work (`ecuIdTabs()`) begins.
3. Profile with a sampling profiler (e.g. Visual Studio Performance Profiler / `perf`) across the button click; the hot frame will be `extractEcuIds()`'s message-decode loop, entirely on the UI thread.

**Suggested fix:** move `extractEcuIds()`'s scan to a background thread (`QtConcurrent::run` + a `QFutureWatcher`, consistent with the pattern already used for Find-All), report real progress from that scan, and delete the dummy `for` loop in `MainWindow` entirely.

### 3.6 Time-range search excludes exact boundary timestamps (`LOGIC-01`) — Low/Medium, correctness not perf, but affects test validity

**Where:** [qdlt/dltmessagematcher.cpp](../qdlt/dltmessagematcher.cpp) L117, `matchTimeRangeMs()` uses strict `<`/`<` instead of `<=`/`<=`, so messages exactly at the configured start/end timestamp are silently dropped from Find-All/search results. Not itself a performance issue, but relevant here because it means any before/after performance comparison of the search path that also checks result-count correctness (e.g. a regression test asserting N matches) can show a false "fewer matches" delta unrelated to the parallelization change. Worth fixing alongside §3.4 while touching this code.

**Suggested fix:** change to `startMsSinceEpoch <= msSinceEpoch && msSinceEpoch <= endMsSinceEpoch`.

### 3.7 `ProjectionTableModel::onSourceDataChanged` does an O(rows) linear scan per source change (Low)

**Where:** [src/projectiontablemodel.cpp](../src/projectiontablemodel.cpp) L85-112: for every `dataChanged` from the source model (i.e. potentially once per incoming live message), it linearly scans the entire `m_projectionRows` vector to find the projected row range to re-emit, and the emitted range can span rows that didn't actually change if the projection is sparse relative to the source range.

**Impact:** for CRLF/ECU-grouped views with a large projection open during high-rate live logging, this adds an O(rows-in-view) cost per incoming message batch, and can cause extra repaints beyond what changed.

**How to test:** open a large ECU-grouped or CRLF view, start high-rate live logging, and profile `ProjectionTableModel::onSourceDataChanged` CPU share; compare against the size of `m_projectionRows`.

**Suggested fix:** maintain a sorted index or hash of `m_projectionRows` for O(log n) range lookup instead of a full linear scan, and consider batching/coalescing `dataChanged` for a live-append burst instead of one call per source signal.

### 3.8 Export routes every message through the shared, mutex-protected decode cache instead of the single-pass bypass it already has access to (High)

**Where:** [qdlt/qdltexporter.cpp](../qdlt/qdltexporter.cpp), `QDltExporter::exportMessages()`:

```cpp
if (hasGlobalIndex && decodeCacheService.message(from, pluginManager, index,
                                                 decodeEnabled, silentMode,
                                                 decoded, true))          // useCache=true, singlePassBypass defaults to false
```

`CDecodeCacheService::message()` (see [qdlt/decodecacheservice.cpp](../qdlt/decodecacheservice.cpp) L31-107) already has a `singlePassBypass` parameter specifically designed for "each message is accessed only once" workloads — it is used correctly elsewhere (indexing, marker counting) to skip the mutex/hash-map entirely. Export is by definition a single forward pass over the file with no message ever revisited, yet it calls `message(...)` with `singlePassBypass` left at its default `false`. Every exported message therefore: builds a `CacheKey` (pointer + 3 ints + generation), takes `std::lock_guard<std::mutex> m_cacheLock`, does an `unordered_map::find`, and on miss inserts into the map + a FIFO deque and runs `pruneIfNeeded()` once the 512-entry cap is hit — all for a cache whose hit rate is structurally 0% during export.

**2.28.1 comparison:** the old export path called `pluginManager->decodeMsg(msg, silentMode)` directly — no cache, no lock, no map. This is a pure-overhead regression introduced by the Single Data Model refactor, not present in 2.28.1.

**How to test:** export a large file (≥1M messages) to "Decoded DLT"/CSV with plugins enabled, and time it before/after passing `true` for `singlePassBypass` at this call site (or instrument with `QElapsedTimer` around the loop / a sampling profiler — `std::mutex::lock`, `std::unordered_map::find/emplace` should show up as non-trivial cumulative time attributable to `CDecodeCacheService::message`).

**Suggested fix:** pass `singlePassBypass=true` from `QDltExporter::exportMessages()` (export never benefits from caching decoded messages, and doing so also avoids growing the shared cache with entries that are never reused, which would otherwise evict genuinely-reusable entries used by search/render).

### 3.9 Export copies message bytes twice via MessageStore indirection (Medium)

**Where:** [qdlt/qdltexporter.cpp](../qdlt/qdltexporter.cpp) `QDltExporter::getMsg()` now does, for every message:

```cpp
const std::vector<char> raw = messageStore.rawMessage(messageId);
buf = QByteArray(raw.data(), static_cast<int>(raw.size()));
```

and [qdlt/messagestore.cpp](../qdlt/messagestore.cpp) `rawMessage()` itself does:

```cpp
const QByteArray data = m_file->messageBytesAt(static_cast<int>(messageId));
return std::vector<char>(data.cbegin(), data.cend());   // copy #1: QByteArray -> std::vector<char>
```

so the round trip is `QByteArray` (from `messageBytesAt`) → **copy** into `std::vector<char>` → **copy** back into `QByteArray` in the exporter. 2.28.1's equivalent (`buf = from->getMsg(num);` / `from->getMsgFilter(num)`) produced the `QByteArray` directly with no intermediate `std::vector` stage.

**Impact:** two extra heap allocations + full-payload memcpys per exported message. For `FormatDlt` (raw binary export, the common bulk-export case) this buffer is the *entire* useful output of the per-message work, so the added copies are pure overhead proportional to total exported bytes.

**How to test:** export a large file to raw DLT format (`FormatDlt`) and compare wall-clock time / allocator call counts (e.g. Visual Studio Heap Profiler, or `perf record`/`valgrind --tool=massif` on Linux) against instrumented builds with and without the `std::vector<char>` round trip.

**Suggested fix:** add a `QByteArray`-returning accessor (e.g. `CMessageStore::rawMessageBytes()`Qt-native) so the adapter can return `m_file->messageBytesAt(...)` directly without the `std::vector<char>` intermediary, or have `QDltExporter::getMsg()` fetch raw bytes straight from `from->messageBytesAt()`/`from->getMsg()` and only use the `MessageStore` abstraction for the decoded `QDltMsg` path.

### 3.10 Redundant row-index resolution computed twice per exported row (Low)

**Where:** [qdlt/qdltexporter.cpp](../qdlt/qdltexporter.cpp) — for `SelectionFiltered`/`SelectionSelected`, `getMsg()` already resolves `messageStore.messageIdForFilteredRow(...)` → `globalIndexForMessageId(...)` to set `msg.setIndex(...)`. Immediately afterward, `exportMsg()` calls `globalIndexForSelectionRow(num)` again for the Text/CSV/Clipboard formats' row-number column — repeating the exact same two-step lookup for the same `num` on the same call.

**Impact:** minor per-row CPU duplication (not per-byte, so smaller than §3.9), but scales linearly with exported row count.

**How to test:** export a large file to CSV and profile `globalIndexForSelectionRow`/`messageIdForFilteredRow` call counts — should be ~2x the exported row count instead of 1x.

**Suggested fix:** have `getMsg()` return (or cache on `this`) the resolved global index, and have `exportMsg()` reuse it instead of recomputing.

### 3.11 `FormatDltDecoded` self-check re-parse added per exported non-verbose message (Low)

**Where:** [qdlt/qdltexporter.cpp](../qdlt/qdltexporter.cpp) `createDltMessage()`:

```cpp
// Self-check: can we parse what we just generated?
QDltMsg checkMsg;
checkMsg.setMsg(out, true, true);
```

The result of `checkMsg` is never used (not returned, not logged conditionally) — this fully re-parses the just-built message purely as a debug-style self-check, unconditionally, in production builds, for every non-verbose message exported with "Decoded DLT" format.

**How to test:** export a large non-verbose-heavy file to "Decoded DLT" format and compare timing with/without this block removed.

**Suggested fix:** delete the dead `checkMsg` self-check, or wrap it in `#ifdef QT_DEBUG` if it's considered valuable during development.

### 3.12 Search now pays decode-cache mutex/hash overhead 2.28.1 never had (Medium)

**Where:** [src/searchdialog.cpp](../src/searchdialog.cpp) `findMessages()` (interactive Find Next/Previous) calls `m_decodeCacheService->message(file, pluginManager, globalIndex, decodeEnabled, fSilentMode, msg, true)` for **every candidate message**, and the non-decode Find-All path (`startParallelFindAll`) reads messages directly (good — no cache there), but the decode-required Find-All path also funnels through the same shared cache.

**2.28.1 comparison:** `findMessages()` called `file->getMsgFilter(searchLine)` directly — a plain indexed file/array access with no mutex, no hash map, no eviction bookkeeping.

**Impact:** for a single Find-Next/Find-Previous step this is a small, likely-imperceptible overhead. It becomes noticeable in two cases: (a) a search/scan that touches more distinct messages than the cache's fixed 512-entry cap (`kMaxEntries` in `decodecacheservice.h`), causing continuous FIFO eviction + reinsertion ("thrashing") with no net benefit, since a linear Find-All scan never revisits a message; (b) this shared cache is the same one used by the main table view and CRLF/ECU views, so a large search run can evict entries those other views were relying on, and vice versa — a search can make the main table's next repaint slower (cache miss) even though nothing about the visible rows changed.

**How to test:**

1. Load a file with well over 512 distinct messages, run Find-All with plugins/payload search enabled (decode path) and profile `CDecodeCacheService::message`/`pruneIfNeeded` share of total time.
2. Compare cache hit/miss counts (add temporary counters, or watch via a debugger conditional breakpoint) during a single linear Find-All pass — expect ~0% hit rate, confirming the cache provides no benefit for this access pattern while still paying its bookkeeping cost.
3. With a search running, scroll the main table view and check whether previously-fast repaints (cache hits) start missing/slowing down due to eviction caused by the concurrent search.

**Suggested fix:** use `singlePassBypass=true` for Find-All's decode-required path (mirroring the §3.8 export fix) since a linear scan never revisits a row; keep caching only for `findMessages()` (Find Next/Previous), which *does* legitimately revisit the same neighborhood of messages repeatedly and benefits from the cache. Consider a larger or workload-aware cache size if search and rendering are found to contend in practice.

### 3.13 Main table lost its cheap one-entry row cache in favor of the shared, mutex-protected cache (High)

**Where:** [src/tablemodel.cpp](../src/tablemodel.cpp) `CTableModel::data()` (~L83-99). Every call, for every requested `Qt::ItemDataRole`, does:

```cpp
const int filterposindex = resolveGlobalIndexForRow(index.row());   // cheap, O(1) via m_filteredProjectionCache
...
if (m_decodeCacheService && m_decodeCacheService->message(qfile, pluginManager, filterposindex,
                                                          decodeEnabled, triggeredByUser, omsg, true))
```

Per the PR #827 review, pre-refactor `TableModel` had "a one-entry message cache to avoid repeating `getMsg()` and plugin decoding for the same row during a paint pass." That cache no longer exists anywhere in `tablemodel.cpp` (confirmed by inspection — no `m_cache`/one-entry member remains). Qt views normally request several roles per cell per repaint (`DisplayRole`, `ForegroundRole`, `BackgroundRole`, etc.), so this is now several `std::mutex` lock + `unordered_map::find` calls per visible cell per repaint, instead of one cheap unsynchronized lookup.

**Impact:** this is the single most frequently exercised path in the entire application — simply scrolling or letting live logging redraw the view pays this cost continuously, unlike search/export which only run on user action.

**How to test:** open a large file, scroll continuously (or let live logging redraw at the configured refresh rate) while sampling-profiling the process; compare `CDecodeCacheService::message` call count against (visible rows × roles requested), and compare against a 2.28.1 build's `TableModel::data()` call graph for the same interaction.

**Suggested fix:** reintroduce a one-entry (or small N-entry) row cache local to `CTableModel`, checked first; only fall through to `m_decodeCacheService` on a miss, matching the design intent of the pre-refactor code without giving up the shared-cache benefit across features.

### 3.14 Global plugin-decode mutex serializes every decode-using feature (Medium)

**Where:** [qdlt/qdltpluginmanager.cpp](../qdlt/qdltpluginmanager.cpp) — `decodeMutex` is one class-level mutex taken by `decodeMsg()`, `decodeMsgTry()`, and `decodeMsgUsingPlugins()`, i.e. every decode call from every caller.

This mutex did not exist in 2.28.1 because decode only ever happened on the UI thread (single-threaded by construction, no concurrent callers, no lock needed). The Single Data Model refactor introduced multiple threads that can call decode concurrently (parallel search, background export, live-log ingestion), which correctly requires *some* serialization since plugin implementations are stateful and not thread-safe — but the chosen mechanism is one global lock shared by every feature, not scoped per plugin instance or per logical operation.

**Impact:** a decode-heavy operation in any one feature (large export, decode-enabled Find-All) now measurably contends with every other feature that decodes at the same time (main table rendering, live logging, CRLF/ECU views) — a form of cross-feature slowdown that was structurally impossible in 2.28.1.

**How to test:** start a large decode-enabled export or Find-All in the background; simultaneously scroll the main table and/or run live logging; measure frame time / responsiveness with and without the concurrent background operation, and compare the delta against the same test on a 2.28.1 build (where no such interaction exists).

**Suggested fix:** this needs a design decision, not a local patch — options include per-plugin-instance locks (only serialize calls into the *same* plugin), a documented single-writer/thread-confinement contract enforced by the host, or accepting the shared lock but auditing call sites so bulk/background operations (export, Find-All) never hold it for the full-file duration in a way that starves interactive paths.

### 3.15 CRLF Filter window shares ECU grouping's synchronous UI-thread scan pattern (Medium)

**Where:** [src/crlffilterwindow.cpp](../src/crlffilterwindow.cpp) `buildCrlfProjectionRows()` (~L620-695): a full linear scan + decode of every filtered message, synchronously, on the calling (UI) thread, on every CRLF window open/rebuild.

This is the same shape as §3.5 (ECU grouping), with two differences: it has a real progress bar (not a fake one) and periodic `QCoreApplication::processEvents()`, and the reentrancy crash risk originally flagged for this code (`REENT-01`: the main window or file being closed/swapped while `processEvents()` is reentered) has been **mitigated** — the code now captures `m_dltFile` up front and checks `m_dltFile != capturedDltFile` after each `processEvents()` call, breaking out if the file was swapped or torn down mid-scan. The crash risk is addressed; the UI-thread stall on a cold cache / large filtered set is not.

**How to test:** open a large file with a large filtered/CRLF-eligible result set, open the CRLF Filter window (or trigger a rebuild) with a cold decode cache, and observe UI responsiveness during the scan; confirm the mitigation by closing the main window or swapping the file while the CRLF progress dialog is active and verifying no crash (only a clean early exit).

**Suggested fix:** move the scan to a background thread with a `QFutureWatcher` (same pattern recommended for §3.5), reporting real progress from the worker instead of via a UI-thread loop.

---

## 4. Priority summary

| Priority   | Finding                                                                             | Category                                         | Where                                                     |
| ---------- | ----------------------------------------------------------------------------------- | ------------------------------------------------ | --------------------------------------------------------- |
| High       | §3.1 Redundant full second file scan in`createIndex()`/`calculateTotalSizes()` — ✅ Fixed | Load-time regression (extra O(n) I/O+parse pass) | `qdlt/qdltfile.cpp`                                     |
| High       | §3.2 Unsynchronized`getIndexFilterRef()` iteration                               | Data race / crash risk under load                | `qdlt/qdltfileprojection.cpp`                           |
| High       | §3.3 Null`m_decodeCacheService` deref                                            | Crash risk on search/render hot path             | `src/searchtablemodel.cpp`, `src/searchdialog.cpp`    |
| High       | §3.4`QThreadStorage<QFile*>` leak per Find-All run                               | Steady memory growth                             | `src/searchdialog.cpp`                                  |
| Medium     | §3.5 Synchronous ECU-ID scan + fake progress bar                                   | UI-thread stall on large files                   | `src/filtergrouplogs.cpp`, `src/mainwindow.cpp`       |
| Low/Medium | §3.6 Time-range boundary exclusion                                                 | Correctness (confounds perf regression tests)    | `qdlt/dltmessagematcher.cpp`                            |
| Low        | §3.7 O(rows) scan per`dataChanged` in projection views                           | Live-logging repaint overhead                    | `src/projectiontablemodel.cpp`                          |
| High       | §3.8 Export bypasses the single-pass cache-bypass it already supports              | Export regression (avoidable lock/hash per msg)  | `qdlt/qdltexporter.cpp`                                 |
| Medium     | §3.9 Export double-copies message bytes via MessageStore                           | Export regression (2 extra copies per msg)       | `qdlt/qdltexporter.cpp`, `qdlt/messagestore.cpp`      |
| Low        | §3.10 Redundant row-index resolution per exported row                              | Export minor CPU duplication                     | `qdlt/qdltexporter.cpp`                                 |
| Low        | §3.11 Discarded self-check re-parse in`createDltMessage()`                       | Export minor CPU waste                           | `qdlt/qdltexporter.cpp`                                 |
| Medium     | §3.12 Search pays decode-cache overhead 2.28.1 never had                           | Search regression risk (thrashing/contention)    | `src/searchdialog.cpp`, `qdlt/decodecacheservice.cpp` |
| High       | §3.13 Main table lost its one-entry row cache                                      | Rendering regression (most-used path)            | `src/tablemodel.cpp`                                    |
| Medium     | §3.14 Global plugin-decode mutex cross-feature contention                          | Design-level concurrency trade-off               | `qdlt/qdltpluginmanager.cpp`                            |
| Medium     | §3.15 CRLF window synchronous UI-thread scan                                       | UI-thread stall on large files (crash mitigated) | `src/crlffilterwindow.cpp`                              |

No repository benchmark target exists for these paths today (no Google Benchmark integration found under `qdlt/`/`src/`). All test/repro steps above use wall-clock timing, OS-level resource monitors, or targeted profiler sampling as the narrowest available validation, since that is what the existing codebase supports.

---

## 5. Root cause: what the Single Data Model refactor intended vs. what shipped

**Stated design intent (from Jira story STSUITE-716, quoted in [PR_822_Code_Review.md](../PR_822_Code_Review.md) §1.1):**

> Use a single data structure to be used/accessed by UI models. Understand the impact. Test with all use-cases, explicitly including live logging, search, filtering, and plugins. Deliver only if there is no or low user impact.

The goal was architecturally sound and explicitly performance-motivated: replace several **independent, feature-local, ad-hoc caches and decode paths** (the table model's one-entry cache, search's own file access, CRLF/ECU-grouping's own re-decode loops, the exporter's own `getMsg`) with **one shared, consistent decode/index/message-store layer**, so the same message decoded for one feature could be reused by another instead of every feature re-parsing/re-decoding it independently. The Jira breakdown (`STSUITE-790`/`791`/`792` design+implementation, `STSUITE-848` "test and collect performance data", `STSUITE-793` PR/review) shows performance validation was explicitly planned as its own step, not an afterthought.

**Why the regressions in §3.8–§3.15 happened anyway:**

1. **Consolidation replaced N cheap single-consumer caches with 1 synchronized multi-consumer cache, without auditing every call site against the old cost.** A shared cache is a genuine win when two features access the *same* message around the same time (e.g. search and the table view). It is pure overhead when a feature's access pattern never benefits from sharing — a single sequential export pass, or repeated same-row/different-role queries from one view during one repaint. The design added exactly the right escape hatch for this (`singlePassBypass`, confirmed correctly used in the indexer/marker-counting code), but it was applied where the *first* bug was found, not systematically rolled out to every consumer with a single-pass or single-consumer access pattern (export, decode-required Find-All, and the table model's per-role re-query never got it, or never got the local cache back).
2. **The review process optimized for correctness and crash-safety, not micro-performance, under real time pressure.** The Phase 1–7 work was merged once, **fully reverted**, then re-merged, then patched three more times (`248b7cbb`, `dabdf104`, `f2e0e22b`). Each of the six local PR reviews in this repo's own [doc/](.) history ([PR_822](../PR_822_Code_Review.md), [PR_823](PR_823_phase5_append_fix_review_2026-08-12.md), [PR_825](PR_825_phase3_search_delink_review_2026-08-21.md), [PR_826](PR_826_phase2_live_log_delink_review_2026-09-08.md), [PR_827](PR_827_phase4_render_cache_delink_review_2026-08-20.md), [PR_840](PR_840_code_review_2026-09-14.md)) did catch and drive fixes for genuinely severe issues — deadlocks, data races, stale/incorrect cached values, a broken periodic UI-refresh timer. Those are the kind of bugs that show up immediately in manual/functional testing. The issues in this document (missing `singlePassBypass`, lost one-entry row cache, global mutex contention) don't break functionality or produce wrong output — they only show up as *measurably slower* under profiling, which is a fundamentally different (and easier to skip under deadline pressure) validation activity than "does the feature still work correctly."
3. **`STSUITE-848` ("test and collect performance data") was closed without an automated/instrumented benchmark.** There is no Google Benchmark (or equivalent) target anywhere in this repository for these code paths. Every review doc that touches performance says so explicitly, in almost the same words each time — e.g. PR #827's review: *"The performance impact requires measurement, but the behavior has not changed."* Without a repeatable, per-call-site benchmark, "collect performance data" in practice likely meant coarse, manual, end-to-end timing of a few scenarios (e.g. "does Find-All still complete in reasonable time on a sample file") rather than verifying that every new indirection layer (MessageStore → DecodeCacheService → QDltFile) costs no more than the code path it replaced for *each* calling feature's specific access pattern.
4. **The global decode mutex (§3.14) is a legitimate, unavoidable *consequence* of the design goal, not an oversight** — once decode can happen from multiple threads (a prerequisite for the parallel search / background export the design also wanted), *some* serialization of stateful plugin calls is required for correctness. The design didn't get this wrong so much as it didn't document or revisit the concurrency-model trade-off (one global lock vs. finer-grained locking) as a deliberate, called-out decision — it's the one item here that needs a design conversation rather than a local code fix.

**In short:** the single-data-model consolidation itself was the right call and the doc's §2.2 "positive impact" list is real — shared decode cache, cache-bypass heuristics, parallel Find-All are genuine wins where the access pattern matches what the cache was built for. The regressions came from **incomplete rollout of the design's own stated remedies** (the bypass flag, the local one-entry cache) to every consumer that needed them, compounded by a review process that (understandably, given the revert-and-redo history) spent its available time on correctness/crash fixes rather than the performance-measurement step the Jira story explicitly called for.

### 5.1 Per-feature summary: what changed and its impact

| Feature                                                    | What the Single Data Model refactor changed                                                                                                                                                                                                                                                                                | Impact                                                                                                                                                                                                                                                                                              | Related findings                    |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| **Main table view** (scroll/render)                  | `CTableModel::data()` moved from a local one-entry per-row message cache to calling the shared `CDecodeCacheService` for every requested role                                                                                                                                                                          | **Negative** — most-used path in the app now pays mutex+hash overhead multiple times per row per repaint instead of once                                                                                                                                                                     | §3.13                              |
| **Find Next / Find Previous**                        | `findMessages()` moved from direct `file->getMsgFilter()` to `SearchProjectionSnapshot` + `CDecodeCacheService::message()`                                                                                                                                                                                         | **Negative for linear/non-revisited scans, neutral-to-positive for back-and-forth navigation** — cache helps when the same rows are revisited, pure overhead when they aren't                                                                                                                | §3.12                              |
| **Find All (no decode needed)**                      | New`SearchProjectionSnapshot` + `QThreadPool`-based chunked parallel scan via `QtConcurrent::blockingMappedReduced`                                                                                                                                                                                                  | **Positive** — 2.28.1 had zero parallelism here; this is strictly faster on multi-core machines                                                                                                                                                                                              | §2.2                               |
| **Find All (decode needed, plugins+payload search)** | Parallel I/O load per chunk, then serial decode+match per chunk (plugin thread-safety constraint); routes through shared decode cache without`singlePassBypass`                                                                                                                                                          | **Neutral vs. 2.28.1 for the decode step itself (still serial either way), but adds cache overhead 2.28.1 never had, plus a confirmed `QThreadStorage<QFile*>` leak per run**                                                                                                               | §3.4, §3.12                       |
| **Export** (all formats)                             | `QDltExporter` now resolves rows via `MessageStore` (`messageIdForFilteredRow`/`globalIndexForMessageId`), fetches bytes via `rawMessage()` (extra `std::vector<char>` round trip), and decodes via the shared cache without `singlePassBypass`; added a new "Decoded DLT" format with a self-check re-parse | **Negative** — every finding here is pure added overhead with no offsetting win; most-suspected cause of the reported export slowdown                                                                                                                                                        | §3.1, §3.8, §3.9, §3.10, §3.11 |
| **CRLF Filter window**                               | Migrated from per-message`QStandardItemModel` duplication to a projection-based model (`ProjectionTableModel`/row-reference projection) reading through the shared decode cache; added a reentrancy guard against the owning file being swapped mid-scan                                                               | **Mixed** — avoids duplicating message data into a second model (real memory/copy win), but the rebuild scan is still synchronous on the UI thread, and it consumes the same unsynchronized filter-index read used elsewhere                                                                 | §3.2, §3.15                       |
| **Group DLT Logs by ECU**                            | `extractEcuIds()` now iterates `messageStore->snapshotAllMessageIds()` and decodes every message synchronously; a legacy fallback path still opens a second `QDltFile` and calls `createIndex()` (full re-index + full re-scan) when the shared services aren't wired up                                           | **Negative** — synchronous full-file decode on the UI thread behind a fake (non-functional) progress bar; the fallback path duplicates an entire file open+index+stats pass                                                                                                                  | §3.1, §3.5                        |
| **Live logging / indexing** (`QDltFile`)           | Added cache admission heuristics (`shouldUseMessageCache`, `cacheSinglePassBypass`, sequential-scan detection), mutex locking around previously-unlocked mutators, a `searchSnapshotGeneration` counter, and per-file size statistics computed via a second full pass in `createIndex()`                           | **Mixed** — the cache-bypass heuristics and locking fixes are real correctness/perf wins for bulk scans; the extra size-statistics pass is a real load-time regression                                                                                                                       | §3.1                               |
| **Plugin decode pipeline**                           | Added a single class-level`decodeMutex` in `QDltPluginManager` to serialize decode calls now that they can originate from multiple threads, plus a `decodePipelineGeneration` counter so caches can detect a changed plugin configuration                                                                            | **Mixed** — necessary and correct for thread-safety once decode became concurrent (2.28.1 never needed this because decode was UI-thread-only); the cost is that it now serializes every decode-using feature against every other one, a contention point 2.28.1 structurally could not have | §3.14                              |
| **Search results table** (rendering)                 | `CSearchTableModel::data()` calls the shared decode cache directly for `DisplayRole`/`ForegroundRole`/`BackgroundRole`, with no null-guard on the cache pointer                                                                                                                                                    | **Negative** — same per-role repeated-lookup shape as the main table (§3.13), plus a null-pointer crash risk if the cache service isn't injected                                                                                                                                            | §3.3, §3.13 (same pattern)        |

### 5.2 Recommended fix plan

Ordered by recommended implementation sequence: quick wins first (small, isolated, low-risk), then medium-effort fixes, then the one item that needs a design decision rather than a local patch. "Size" matches the estimates already used in the quick-reference table.

#### Tier 1 — Trivial/Small (do these first; each is a localized, low-risk change)

| Fix    | What to do                                                                                                                         | Size             | Feature(s) it improves                                                                                                                      |
| ------ | ---------------------------------------------------------------------------------------------------------------------------------- | ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| §3.13 | Reintroduce a one-entry "last row" message cache in`CTableModel`, checked before falling through to `m_decodeCacheService`     | Small            | **Main table** (scroll/render) — highest-traffic path in the app, likely the single biggest win for general perceived responsiveness |
| §3.8  | Pass`singlePassBypass=true` at the one `decodeCacheService.message(...)` call site in `QDltExporter::exportMessages()`       | Small (one-line) | **Export** — removes a mutex+hash op per exported message                                                                            |
| §3.3  | Add`if (m_decodeCacheService && ...)` guards around the ~3 unguarded call sites in `searchtablemodel.cpp`/`searchdialog.cpp` | Trivial          | **Search / search results table** — removes a null-pointer crash risk                                                                |
| §3.4  | Replace the per-call`QThreadStorage<QFile*>` in `startParallelFindAll()` with a plain per-chunk `QFile`                      | Small            | **Search (Find-All)** — stops a steady memory/handle leak on repeated searches                                                       |
| §3.6  | Change`matchTimeRangeMs()` comparisons from `<`/`<` to `<=`/`<=`                                                         | Trivial          | **Search** — fixes dropped boundary-timestamp matches (also removes a confound from any future perf regression test)                 |
| §3.10 | Compute the resolved global index once in`getMsg()`/`exportMsg()` and reuse it instead of recomputing                          | Trivial          | **Export** (Text/CSV/Clipboard)                                                                                                       |
| §3.11 | Delete the dead`checkMsg` self-check re-parse in `createDltMessage()` (or gate behind `QT_DEBUG`)                            | Trivial          | **Export** ("Decoded DLT" format)                                                                                                     |
| §3.2  | Swap`getIndexFilterRef()` for the existing locked copy accessor (`getIndexFilter()`) in `buildActiveFilteredProjection()`    | Small            | **CRLF Filter window / ECU grouping** — removes a data-race/crash risk under concurrent live logging or filter rebuild               |

#### Tier 2 — Medium (still localized, but touch more code / need a bit more care)

| Fix    | What to do                                                                                                                                              | Size   | Feature(s) it improves                                                                                                                    |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------------------- |
| §3.1  | ~~Fold the size-statistics accounting (`calculateTotalSizes()`) into the existing single-pass `updateIndex()` loop instead of a dedicated second pass~~ **Done** | Medium | **File open/indexing, `commander` CLI export, ECU-grouping fallback path** — all now scan the file once instead of twice |
| §3.12 | Use`singlePassBypass=true` for the decode-required Find-All path (mirrors §3.8); keep caching only for `findMessages()` (Find Next/Previous)       | Medium | **Search (Find-All with plugins/payload search enabled)**                                                                           |
| §3.9  | Add a`QByteArray`-returning accessor on the message-store adapter so raw bytes don't round-trip through `std::vector<char>`                         | Medium | **Export** (all formats, especially raw `FormatDlt`)                                                                              |
| §3.7  | Add a sorted/hash index alongside`ProjectionTableModel::m_projectionRows` for O(log n) lookup instead of a linear scan per `dataChanged`            | Medium | **CRLF Filter window / ECU-grouped views** during high-rate live logging                                                            |
| §3.15 | Move`CrlfFilterWindow::buildCrlfProjectionRows()`'s scan to a background thread with a `QFutureWatcher` (mirroring the Find-All pattern)            | Medium | **CRLF Filter window** — removes the UI-thread stall on cold cache/large filtered sets                                             |

#### Tier 3 — Larger effort / needs a design decision

| Fix    | What to do                                                                                                                                                                           | Size                                                    | Feature(s) it improves                                                                                                                                                      |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| §3.5  | Move`extractEcuIds()`'s scan to a background thread (`QtConcurrent::run`/`QFutureWatcher`) with real progress reporting; delete the dummy `for` loop in `MainWindow`       | Medium/Large                                            | **Group DLT Logs by ECU** — removes the UI freeze and the misleading fake progress bar                                                                               |
| §3.14 | Decide and implement a finer-grained decode concurrency model (e.g. per-plugin-instance locks, or a documented/enforced single-writer contract) instead of one global`decodeMutex` | Medium/Large (needs a concurrency-model decision first) | **All decode-using features simultaneously** (live logging, main table, search, export, CRLF, ECU grouping) — removes cross-feature contention that 2.28.1 never had |

**If you can only do three things:** §3.13 (main table — constantly exercised), §3.8 (export — a one-line fix directly addressing the reported slow export), and §3.1 (load-time — benefits every feature that opens a file, including export and ECU grouping). Together these are the lowest-effort, highest-reach set.

---

## 6. Summary of changes per release

### 2.28.1 → 2.30.0 (189 commits)

- **Search:** multithreaded Find-All via `QtConcurrent` (#760), matcher short-circuiting, search-index handling revamped (search-by-time, aggregate filter info), a lazy filter-index-start optimization was tried and reverted (`38da3b3f`) after regressing behavior.
- **Message formatting:** `toStringHeader`/`toStringPayload` caching (`70f3cab4`) — real allocation/CPU win on render/export.
- **Live logging:** marker-recompute skip when no manual markers (`4ecfb0ca`); CFI skip when no filter active.
- **Indexing:** new per-file storage/message/payload size statistics computed during indexing (`a216b87b`) — functionally useful, but the implementation basis for the §3.1 double-scan regression that persists into 2.31.0.
- **Features (non-perf):** DLTv2 raw-file support, ECU-based filtering/grouping (first version), record-size display, export search results to DLT, CRLF/marker fixes, Check-for-Update, CMake-only packaging (qmake removed), CI/build-matrix modernization.

### 2.30.0 → 2.31.0 / master (63 commits)

- **Architecture:** "Single Data Model" refactor delivered in 7 phases — `MessageStore` → `IndexService`/`DecodeCacheService` → incremental `CTableModel` updates → `ProjectionTableModel` for CRLF/ECU views → plugin-contract hardening (transactional decode, serialized plugin calls) → cache-bypass/fast-matcher/parallel Find-All. This full set was merged once, **reverted in its entirety**, then **re-merged** with additional fixes, followed by "Improve filtering and search handling," "Update filter marker checks and refresh logic," and a final "PR Review fix."
- **Net result:** shared decode cache across search/export/derived-views (fewer duplicate plugin-decode calls), cache-bypass heuristics for single-pass bulk scans, cheaper matcher (cached UTF-8 pattern, early exit), and a parallel non-decode Find-All path — real wins for the common cases.
- **Confirmed regressions/risks still on master** (this document, §3): redundant full second-pass file scan on every index build, an unsynchronized read of the live filter index during concurrent access, a null-deref risk on the decode-cache pointer, a per-search `QThreadStorage<QFile*>` leak, a synchronous UI-thread ECU-ID scan with a fake progress bar, an off-by-boundary time-range filter, and an O(rows) linear scan in the new projection model's change-propagation path.
- **Other changes:** DLT file splitting settings, feedback submission dialog, Windows ARM build support, macOS Qt6/offscreen-plugin packaging, centralized configuration settings, CRLF window performance/consistency fixes, plugin pane/CLI configuration fixes, high-volume ECU ingestion crash fixes.
