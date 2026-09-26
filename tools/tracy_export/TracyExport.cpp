// tracy-export — dump EVERYTHING a .tracy capture holds into a folder of CSV
// tables + info.json, for scripts (tools/tracy_report.py and friends) to read
// without the GUI.
//
// Why not tracy-csvexport: it drops frames, GPU contexts, locks, memory,
// context switches, samples and thread names; its message/plot modes are
// partial; and it does not quote fields, so zone text or messages with a
// comma break the row. This tool is built from the SAME Tracy release as the
// viewer (tools/build_tracy_tools.sh reads the pin from CMakeLists.txt), so it
// reads every capture the viewer can save.
//
//     tracy-export capture.tracy -o out_dir [--no-zones] [--no-memory] [--no-samples]
//     tracy-export -V
//
// All times are nanoseconds on the capture's own timeline (the same values
// Tracy's GUI and tracy-csvexport show). Tables, all RFC 4180 CSV:
//
//   info.json         capture metadata, counts, frame-set / GPU-context / plot /
//                     memory-pool summaries, parameters, app info, crash
//   threads.csv       tid, name, fiber flag, zone / message / sample counts,
//                     first / last zone time and top-level busy time,
//                     scheduler time when context switches were captured
//   srclocs.csv       every source location: name, function, file, line, colour
//   zone_stats.csv    per zone name × thread, and per name over all threads
//                     (thread = "all"): count, total, self total, mean, min,
//                     p50/p90/p99, max, std, self mean/max
//   frames.csv        every complete frame of every frame set (FrameMark /
//                     FrameMarkNamed)
//   frame_zones.csv   per frame set × frame × thread × zone name: count, total,
//                     EXACT self and max — a zone belongs to the frame its
//                     start falls in. The per-frame / per-tick breakdown
//                     without reading the raw zones
//   zones/<tid>_<thread>.csv  every CPU zone of that thread, pre-order (so
//                     sorted by start): id, parent id, depth, start, end,
//                     EXACT self time, name, srcloc, text (ZoneText /
//                     ZoneValue), dynamic name (ZoneName). Large — tens of
//                     millions of rows for a minute of play; --no-zones skips it
//   gpu_zones.csv     every GPU zone: context, CPU submit window, GPU window
//   gpu_stats.csv     per GPU zone name × context
//   plots.csv         every plot sample (TracyPlot, memory, CPU usage, power)
//   messages.csv      every message with thread, source, severity, colour
//   locks.csv         per lock: acquisitions, hold and wait totals / max, contention
//   lock_events.csv   every lock event
//   memory.csv        every allocation per pool (alloc/free time + thread)
//   context_switches.csv  per-thread scheduling slices (Linux/Windows captures)
//   samples.csv       every callstack sample: thread, leaf frame, root→leaf stack
//   samples_folded.txt    the samples as folded stacks (flamegraph.pl / speedscope)
//
// Exit codes: 0 ok, 1 usage / IO, 2 capture from a NEWER Tracy (rebuild the
// tools at the viewer's version), 3 legacy capture (run tracy-update), 4 not a
// Tracy file / read error.

#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"
#include "TracyVersion.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

    using tracy::Worker;

    // ── Output helpers ─────────────────────────────────────────────────────

    // A CSV table: RFC 4180 quoting (fields holding a comma, quote, CR or LF
    // are quoted, quotes doubled), '\n' row ends, a large stdio buffer.
    class Csv {
    public:
        Csv(const std::filesystem::path& path, std::initializer_list<const char*> header) {
            m_file = std::fopen(path.string().c_str(), "wb");
            if (!m_file) {
                std::fprintf(stderr, "tracy-export: cannot write %s\n", path.string().c_str());
                std::exit(1);
            }
            m_buffer.resize(1 << 20);
            std::setvbuf(m_file, m_buffer.data(), _IOFBF, m_buffer.size());
            for (const char* h : header) Str(h);
            End();
        }
        ~Csv() { if (m_file) std::fclose(m_file); }
        Csv(const Csv&) = delete;
        Csv& operator=(const Csv&) = delete;

        Csv& Str(std::string_view s) {
            Sep();
            if (s.find_first_of(",\"\r\n") == std::string_view::npos) {
                std::fwrite(s.data(), 1, s.size(), m_file);
                return *this;
            }
            std::fputc('"', m_file);
            for (char c : s) {
                if (c == '"') std::fputc('"', m_file);
                std::fputc(c, m_file);
            }
            std::fputc('"', m_file);
            return *this;
        }
        Csv& Str(const char* s) { return Str(std::string_view(s ? s : "")); }
        Csv& Int(int64_t v) { Sep(); std::fprintf(m_file, "%" PRId64, v); return *this; }
        Csv& UInt(uint64_t v) { Sep(); std::fprintf(m_file, "%" PRIu64, v); return *this; }
        Csv& Real(double v) { Sep(); std::fprintf(m_file, "%.17g", v); return *this; }
        Csv& Empty() { Sep(); return *this; }
        Csv& Hex(uint64_t v) { Sep(); std::fprintf(m_file, "0x%" PRIx64, v); return *this; }
        Csv& Color(uint32_t c) {
            Sep();
            if (c != 0) std::fprintf(m_file, "#%06x", c & 0xFFFFFF);
            return *this;
        }
        void End() { std::fputc('\n', m_file); m_first = true; ++m_rows; }
        uint64_t Rows() const { return m_rows - 1; }

    private:
        void Sep() {
            if (!m_first) std::fputc(',', m_file);
            m_first = false;
        }
        FILE* m_file = nullptr;
        std::vector<char> m_buffer;
        bool m_first = true;
        uint64_t m_rows = 0;
    };

    // A minimal JSON writer — enough for info.json.
    class Json {
    public:
        explicit Json(const std::filesystem::path& path) {
            m_file = std::fopen(path.string().c_str(), "wb");
            if (!m_file) {
                std::fprintf(stderr, "tracy-export: cannot write %s\n", path.string().c_str());
                std::exit(1);
            }
        }
        ~Json() { if (m_file) { std::fputc('\n', m_file); std::fclose(m_file); } }

        void BeginObject(const char* key = nullptr) { Key(key); std::fputc('{', m_file); m_first.push_back(true); }
        void EndObject() { m_first.pop_back(); std::fputc('}', m_file); }
        void BeginArray(const char* key = nullptr) { Key(key); std::fputc('[', m_file); m_first.push_back(true); }
        void EndArray() { m_first.pop_back(); std::fputc(']', m_file); }

        void Str(const char* key, std::string_view v) { Key(key); Quoted(v); }
        void Str(const char* key, const char* v) { Str(key, std::string_view(v ? v : "")); }
        void Str(const char* key, const std::string& v) { Str(key, std::string_view(v)); }
        void Int(const char* key, int64_t v) { Key(key); std::fprintf(m_file, "%" PRId64, v); }
        void UInt(const char* key, uint64_t v) { Key(key); std::fprintf(m_file, "%" PRIu64, v); }
        void Real(const char* key, double v) {
            Key(key);
            if (std::isfinite(v)) std::fprintf(m_file, "%.17g", v);
            else std::fputs("null", m_file);
        }
        void Bool(const char* key, bool v) { Key(key); std::fputs(v ? "true" : "false", m_file); }

    private:
        void Key(const char* key) {
            if (!m_first.empty()) {
                if (!m_first.back()) std::fputc(',', m_file);
                m_first.back() = false;
            }
            if (key) { Quoted(key); std::fputc(':', m_file); }
        }
        void Quoted(std::string_view s) {
            std::fputc('"', m_file);
            for (unsigned char c : s) {
                switch (c) {
                    case '"':  std::fputs("\\\"", m_file); break;
                    case '\\': std::fputs("\\\\", m_file); break;
                    case '\n': std::fputs("\\n", m_file); break;
                    case '\r': std::fputs("\\r", m_file); break;
                    case '\t': std::fputs("\\t", m_file); break;
                    default:
                        if (c < 0x20) std::fprintf(m_file, "\\u%04x", c);
                        else std::fputc(c, m_file);
                }
            }
            std::fputc('"', m_file);
        }
        FILE* m_file = nullptr;
        std::vector<bool> m_first;
    };

    // ── Tracy access helpers ───────────────────────────────────────────────

    // Tracy stores a child list either as pointers or, when every child was
    // allocated contiguously, as the events themselves ("magic" vectors).
    template <typename Event, typename F>
    void ForEachEvent(const tracy::Vector<tracy::short_ptr<Event>>& vec, F&& f) {
        if (vec.is_magic()) {
            const auto& direct = *reinterpret_cast<const tracy::Vector<Event>*>(&vec);
            for (const auto& ev : direct) f(ev);
        } else {
            for (const auto& ptr : vec) f(*ptr);
        }
    }

    const char* SrcLocName(const Worker& w, const tracy::SourceLocation& sl) {
        return w.GetString(sl.name.active ? sl.name : sl.function);
    }

    std::string FrameSetName(const Worker& w, const tracy::FrameData& fd) {
        if (fd.name == 0) return "Frames";
        if (fd.name >> 63 != 0) return "[" + std::to_string(uint32_t(fd.name)) + "] Vsync";
        return w.GetString(fd.name);
    }

    const char* GpuContextTypeName(tracy::GpuContextType t) {
        switch (t) {
            case tracy::GpuContextType::OpenGl:     return "OpenGL";
            case tracy::GpuContextType::Vulkan:     return "Vulkan";
            case tracy::GpuContextType::OpenCL:     return "OpenCL";
            case tracy::GpuContextType::Direct3D12: return "Direct3D12";
            case tracy::GpuContextType::Direct3D11: return "Direct3D11";
            case tracy::GpuContextType::Metal:      return "Metal";
            case tracy::GpuContextType::Custom:     return "Custom";
            case tracy::GpuContextType::CUDA:       return "CUDA";
            case tracy::GpuContextType::Rocprof:    return "Rocprof";
            case tracy::GpuContextType::WebGPU:     return "WebGPU";
            default:                                return "Invalid";
        }
    }

    const char* PlotTypeName(tracy::PlotType t) {
        switch (t) {
            case tracy::PlotType::User:    return "user";
            case tracy::PlotType::Memory:  return "memory";
            case tracy::PlotType::SysTime: return "cpu_usage";
            case tracy::PlotType::Power:   return "power";
        }
        return "unknown";
    }

    const char* PlotFormatName(tracy::PlotValueFormatting f) {
        switch (f) {
            case tracy::PlotValueFormatting::Number:     return "number";
            case tracy::PlotValueFormatting::Memory:     return "memory";
            case tracy::PlotValueFormatting::Percentage: return "percentage";
            case tracy::PlotValueFormatting::Watt:       return "watt";
        }
        return "unknown";
    }

    std::string PlotName(const Worker& w, const tracy::PlotData& p) {
        if (p.name != 0) return w.GetString(p.name);
        switch (p.type) {
            case tracy::PlotType::Memory:  return "Memory usage";
            case tracy::PlotType::SysTime: return "CPU usage";
            default:                       return "(unnamed)";
        }
    }

    const char* SeverityName(tracy::MessageSeverity s) {
        switch (s) {
            case tracy::MessageSeverity::Trace:   return "trace";
            case tracy::MessageSeverity::Debug:   return "debug";
            case tracy::MessageSeverity::Info:    return "info";
            case tracy::MessageSeverity::Warning: return "warning";
            case tracy::MessageSeverity::Error:   return "error";
            case tracy::MessageSeverity::Fatal:   return "fatal";
            default:                              return "unknown";
        }
    }

    const char* LockEventName(tracy::LockEvent::Type t) {
        switch (t) {
            case tracy::LockEvent::Type::Wait:          return "wait";
            case tracy::LockEvent::Type::Obtain:        return "obtain";
            case tracy::LockEvent::Type::Release:       return "release";
            case tracy::LockEvent::Type::WaitShared:    return "wait_shared";
            case tracy::LockEvent::Type::ObtainShared:  return "obtain_shared";
            case tracy::LockEvent::Type::ReleaseShared: return "release_shared";
        }
        return "unknown";
    }

    // ── Statistics ─────────────────────────────────────────────────────────

    // Every duration (for the percentiles) plus the self-time running sums.
    struct Samples {
        std::vector<int64_t> total;
        int64_t selfTotal = 0;
        int64_t selfMax = 0;
        bool hasSelf = false;

        void Add(int64_t duration) { total.push_back(duration); }
        void Add(int64_t duration, int64_t self) {
            total.push_back(duration);
            selfTotal += self;
            selfMax = std::max(selfMax, self);
            hasSelf = true;
        }
    };

    int64_t Percentile(const std::vector<int64_t>& sorted, double p) {
        if (sorted.empty()) return 0;
        const size_t idx = std::min(sorted.size() - 1, size_t(p * double(sorted.size())));
        return sorted[idx];
    }

    void WriteStatsRow(Csv& csv, const std::string& name, const std::string& group, Samples& s) {
        auto& t = s.total;
        std::sort(t.begin(), t.end());
        const size_t n = t.size();
        long double sum = 0, sumSq = 0;
        for (int64_t v : t) { sum += v; sumSq += (long double)v * v; }
        const double mean = double(sum / n);
        const double var = n > 1 ? double((sumSq - sum * sum / n) / (n - 1)) : 0.0;
        csv.Str(name).Str(group).UInt(n).Int(int64_t(sum));
        if (s.hasSelf) csv.Int(s.selfTotal); else csv.Empty();
        csv.Real(mean)
           .Int(t.front()).Int(Percentile(t, 0.5)).Int(Percentile(t, 0.9)).Int(Percentile(t, 0.99))
           .Int(t.back()).Real(std::sqrt(std::max(0.0, var)));
        if (s.hasSelf) csv.Real(double(s.selfTotal) / double(n)).Int(s.selfMax);
        else csv.Empty().Empty();
        csv.End();
    }

    // ── Exporters ──────────────────────────────────────────────────────────

    struct Options {
        std::string trace;
        std::filesystem::path out;
        bool zones = true;
        bool memory = true;
        bool samples = true;
    };

    struct Counts {
        uint64_t zones = 0, unfinishedZones = 0, gpuZones = 0, unresolvedGpuZones = 0;
        uint64_t frames = 0, plotPoints = 0, messages = 0, lockEvents = 0, allocations = 0;
        uint64_t contextSwitches = 0, samples = 0;
    };

    void ExportSourceLocations(Worker& w, const std::filesystem::path& dir) {
        // Source location ids run from -(payload count) to (static count - 1).
        Csv csv(dir / "srclocs.csv", {"srcloc", "name", "function", "file", "line", "color"});
        auto emit = [&](int16_t id) {
            const auto& sl = w.GetSourceLocation(id);
            csv.Int(id).Str(SrcLocName(w, sl)).Str(w.GetString(sl.function)).Str(w.GetString(sl.file))
               .UInt(sl.line).Color(sl.color).End();
        };
        const auto& slz = w.GetSourceLocationZones();
        std::vector<int16_t> ids;
        for (const auto& kv : slz) ids.push_back(kv.first);
        for (const auto& kv : w.GetGpuSourceLocationZones()) ids.push_back(kv.first);
        for (const auto& kv : w.GetLockMap()) ids.push_back(kv.second->srcloc);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (int16_t id : ids) emit(id);
    }

    // A frame set's complete frames, for attributing zones to frames.
    struct FrameSpans {
        std::string name;
        std::vector<int64_t> begin, end;
    };

    std::vector<FrameSpans> CollectFrameSpans(Worker& w) {
        std::vector<FrameSpans> sets;
        for (const tracy::FrameData* fd : w.GetFrames()) {
            FrameSpans fs;
            fs.name = FrameSetName(w, *fd);
            // A continuous set's last frame runs to the end of the capture —
            // it never finished, so it is left out; so is an unclosed
            // discontinuous frame.
            size_t n = fd->frames.size();
            if (fd->continuous && n > 0) --n;
            if (!fd->continuous && n > 0 && fd->frames.back().end < 0) --n;
            for (size_t i = 0; i < n; ++i) {
                fs.begin.push_back(w.GetFrameBegin(*fd, i));
                fs.end.push_back(w.GetFrameEnd(*fd, i));
            }
            sets.push_back(std::move(fs));
        }
        return sets;
    }

    // The frame of `set` whose [begin, end) holds `t`, or -1.
    int64_t FrameAt(const FrameSpans& set, int64_t t) {
        auto it = std::upper_bound(set.begin.begin(), set.begin.end(), t);
        if (it == set.begin.begin()) return -1;
        const size_t i = size_t(it - set.begin.begin()) - 1;
        return t < set.end[i] ? int64_t(i) : -1;
    }

    std::string SafeFileName(std::string s) {
        for (char& c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        }
        return s;
    }

    // One walk over every CPU zone writes: zones/<tid>_<name>.csv (the raw
    // rows, optional), zone_stats.csv, frame_zones.csv and threads.csv.
    void ExportZones(Worker& w, const std::filesystem::path& dir, bool writeRows,
                     const std::vector<FrameSpans>& frameSets, Counts& counts) {
        if (writeRows) std::filesystem::create_directories(dir / "zones");

        // Zone names by srcloc, interned to small ids.
        std::vector<std::string> names;
        std::unordered_map<std::string, uint32_t> nameIds;
        std::unordered_map<int16_t, uint32_t> srclocName;
        auto nameIdOf = [&](int16_t srcloc) -> uint32_t {
            auto it = srclocName.find(srcloc);
            if (it != srclocName.end()) return it->second;
            std::string name = SrcLocName(w, w.GetSourceLocation(srcloc));
            auto [nit, inserted] = nameIds.emplace(name, uint32_t(names.size()));
            if (inserted) names.push_back(std::move(name));
            srclocName.emplace(srcloc, nit->second);
            return nit->second;
        };

        // Statistics per (name id, thread index); per-frame sums keyed by
        // frame set | frame | thread index | name id.
        const auto& threads = w.GetThreadData();
        std::map<std::pair<uint32_t, uint32_t>, Samples> byThread;
        struct FrameAcc { uint32_t count = 0; int64_t total = 0, self = 0, max = 0; };
        std::unordered_map<uint64_t, FrameAcc> perFrame;
        auto frameKey = [](uint64_t set, uint64_t frame, uint64_t thread, uint64_t name) {
            return (set << 60) | (frame << 36) | (thread << 20) | name;
        };
        const bool canKeyFrames = frameSets.size() <= 16 && threads.size() <= (1u << 16) &&
            std::all_of(frameSets.begin(), frameSets.end(), [](const FrameSpans& f) { return f.begin.size() < (1u << 24); });

        struct ThreadSpan { int64_t first = INT64_MAX, last = INT64_MIN, busy = 0; };
        std::vector<ThreadSpan> spans(threads.size());

        uint64_t nextId = 0;
        struct Frame { const tracy::ZoneEvent* ev; int64_t parent; int depth; };
        std::vector<Frame> stack;
        for (uint32_t ti = 0; ti < threads.size(); ++ti) {
            const tracy::ThreadData* td = threads[ti];
            std::unique_ptr<Csv> csv;
            if (writeRows && td->count > 0) {
                const std::string file = std::to_string(td->id) + "_" + SafeFileName(w.GetThreadName(td->id)) + ".csv";
                csv = std::make_unique<Csv>(dir / "zones" / file,
                    std::initializer_list<const char*>{"id", "parent", "depth", "start_ns", "end_ns", "self_ns",
                                                       "name", "srcloc", "text", "dyn_name"});
            }
            // Explicit stack, pushed in reverse so rows come out pre-order
            // (sorted by start).
            auto pushChildren = [&](const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& vec,
                                    int64_t parent, int depth) {
                const size_t base = stack.size();
                ForEachEvent<tracy::ZoneEvent>(vec, [&](const tracy::ZoneEvent& ev) {
                    stack.push_back({&ev, parent, depth});
                });
                std::reverse(stack.begin() + base, stack.end());
            };
            pushChildren(td->timeline, -1, 0);
            ThreadSpan& span = spans[ti];
            while (!stack.empty()) {
                const Frame f = stack.back();
                stack.pop_back();
                const tracy::ZoneEvent& ev = *f.ev;
                const int64_t start = ev.Start();
                const int64_t end = w.GetZoneEnd(ev);
                if (!ev.IsEndValid()) ++counts.unfinishedZones;
                int64_t childTime = 0;
                if (ev.HasChildren()) {
                    ForEachEvent<tracy::ZoneEvent>(w.GetZoneChildren(ev.Child()), [&](const tracy::ZoneEvent& c) {
                        childTime += w.GetZoneEnd(c) - c.Start();
                    });
                }
                const int64_t duration = end - start;
                const int64_t self = duration - childTime;
                const int64_t id = int64_t(nextId++);
                const uint32_t nameId = nameIdOf(ev.SrcLoc());
                if (csv) {
                    csv->Int(id).Int(f.parent).Int(f.depth).Int(start).Int(end).Int(self)
                        .Str(names[nameId]).Int(ev.SrcLoc());
                    if (w.HasZoneExtra(ev)) {
                        const auto& extra = w.GetZoneExtra(ev);
                        if (extra.text.Active()) csv->Str(w.GetString(extra.text)); else csv->Empty();
                        if (extra.name.Active()) csv->Str(w.GetString(extra.name)); else csv->Empty();
                    } else {
                        csv->Empty().Empty();
                    }
                    csv->End();
                }
                byThread[{nameId, ti}].Add(duration, self);
                if (f.depth == 0) {
                    span.first = std::min(span.first, start);
                    span.last = std::max(span.last, end);
                    span.busy += duration;
                }
                if (canKeyFrames) {
                    for (size_t si = 0; si < frameSets.size(); ++si) {
                        const int64_t fi = FrameAt(frameSets[si], start);
                        if (fi < 0) continue;
                        FrameAcc& acc = perFrame[frameKey(si, uint64_t(fi), ti, nameId)];
                        ++acc.count;
                        acc.total += duration;
                        acc.self += self;
                        acc.max = std::max(acc.max, duration);
                    }
                }
                ++counts.zones;
                if (ev.HasChildren()) pushChildren(w.GetZoneChildren(ev.Child()), id, f.depth + 1);
            }
        }

        {
            Csv stats(dir / "zone_stats.csv", {"name", "thread", "count", "total_ns", "self_total_ns", "mean_ns",
                                               "min_ns", "p50_ns", "p90_ns", "p99_ns", "max_ns", "std_ns",
                                               "self_mean_ns", "self_max_ns"});
            // byThread is ordered by name id, so each name's threads are
            // adjacent: the per-thread rows, then that name over all threads.
            for (auto it = byThread.begin(); it != byThread.end();) {
                const uint32_t nameId = it->first.first;
                Samples all;
                auto next = it;
                for (; next != byThread.end() && next->first.first == nameId; ++next) {
                    all.total.insert(all.total.end(), next->second.total.begin(), next->second.total.end());
                    all.selfTotal += next->second.selfTotal;
                    all.selfMax = std::max(all.selfMax, next->second.selfMax);
                    all.hasSelf = true;
                    WriteStatsRow(stats, names[nameId], std::to_string(threads[next->first.second]->id), next->second);
                    Samples().total.swap(next->second.total);
                }
                WriteStatsRow(stats, names[nameId], "all", all);
                it = next;
            }
        }

        {
            // Zones attributed to the frame their START falls in, per frame
            // set. Nested zones each count, so sum self_ns (not total_ns)
            // across names to split a frame without double counting.
            Csv csv(dir / "frame_zones.csv", {"frame_set", "frame", "thread", "name", "count", "total_ns",
                                              "self_ns", "max_ns"});
            std::vector<uint64_t> keys;
            keys.reserve(perFrame.size());
            for (const auto& kv : perFrame) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());
            for (uint64_t k : keys) {
                const FrameAcc& acc = perFrame[k];
                const size_t set = size_t(k >> 60);
                const uint64_t frame = (k >> 36) & 0xFFFFFF;
                const size_t ti = size_t((k >> 20) & 0xFFFF);
                const uint32_t nameId = uint32_t(k & 0xFFFFF);
                csv.Str(frameSets[set].name).UInt(frame).UInt(threads[ti]->id).Str(names[nameId])
                   .UInt(acc.count).Int(acc.total).Int(acc.self).Int(acc.max).End();
            }
        }

        Csv csv(dir / "threads.csv", {"tid", "name", "is_fiber", "group_hint", "pid", "zones", "messages", "samples",
                                      "first_ns", "last_ns", "busy_ns", "ctx_running_ns", "ctx_regions",
                                      "ctx_migrations"});
        const auto& cpuThreads = w.GetCpuThreadData();
        for (uint32_t ti = 0; ti < threads.size(); ++ti) {
            const tracy::ThreadData* td = threads[ti];
            csv.UInt(td->id).Str(w.GetThreadName(td->id)).Int(td->isFiber ? 1 : 0).Int(td->groupHint)
               .UInt(w.GetPidFromTid(td->id)).UInt(td->count).UInt(td->messages.size()).UInt(td->samples.size());
            if (spans[ti].first <= spans[ti].last) csv.Int(spans[ti].first).Int(spans[ti].last).Int(spans[ti].busy);
            else csv.Empty().Empty().Empty();
            const auto it = cpuThreads.find(td->id);
            if (it != cpuThreads.end()) {
                csv.Int(it->second.runningTime).UInt(it->second.runningRegions).UInt(it->second.migrations);
            } else {
                csv.Empty().Empty().Empty();
            }
            csv.End();
        }
    }

    void ExportFrames(Worker& w, const std::filesystem::path& dir, const std::vector<FrameSpans>& frameSets,
                      Json& info, Counts& counts) {
        Csv csv(dir / "frames.csv", {"frame_set", "frame", "start_ns", "end_ns", "duration_ns", "has_image"});
        info.BeginArray("frame_sets");
        const auto& all = w.GetFrames();
        for (size_t si = 0; si < frameSets.size(); ++si) {
            const FrameSpans& fs = frameSets[si];
            const size_t n = fs.begin.size();
            std::vector<int64_t> durations;
            durations.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const int64_t d = fs.end[i] - fs.begin[i];
                csv.Str(fs.name).UInt(i).Int(fs.begin[i]).Int(fs.end[i]).Int(d)
                   .Int(all[si]->frames[i].frameImage >= 0 ? 1 : 0).End();
                durations.push_back(d);
            }
            counts.frames += n;
            std::sort(durations.begin(), durations.end());
            long double sum = 0;
            for (int64_t d : durations) sum += d;
            info.BeginObject();
            info.Str("name", fs.name);
            info.Bool("continuous", all[si]->continuous != 0);
            info.UInt("count", n);
            if (n > 0) {
                info.Real("mean_ns", double(sum / n));
                info.Int("min_ns", durations.front());
                info.Int("p50_ns", Percentile(durations, 0.5));
                info.Int("p90_ns", Percentile(durations, 0.9));
                info.Int("p99_ns", Percentile(durations, 0.99));
                info.Int("max_ns", durations.back());
            }
            info.EndObject();
        }
        info.EndArray();
    }

    void ExportGpu(Worker& w, const std::filesystem::path& dir, Json& info, Counts& counts) {
        Csv csv(dir / "gpu_zones.csv", {"id", "parent", "context", "thread", "depth", "cpu_start_ns", "cpu_end_ns",
                                        "gpu_start_ns", "gpu_end_ns", "gpu_ns", "name", "srcloc", "query_id"});
        std::map<std::pair<std::string, uint64_t>, Samples> stats;
        info.BeginArray("gpu_contexts");
        const auto& contexts = w.GetGpuData();
        uint64_t nextId = 0;
        for (size_t ci = 0; ci < contexts.size(); ++ci) {
            const tracy::GpuCtxData* ctx = contexts[ci];
            const std::string ctxName = ctx->name.Active() ? w.GetString(ctx->name)
                                                           : std::string(GpuContextTypeName(ctx->type)) + " context " + std::to_string(ci);
            info.BeginObject();
            info.UInt("index", ci);
            info.Str("name", ctxName);
            info.Str("type", GpuContextTypeName(ctx->type));
            info.UInt("thread", ctx->thread);
            info.UInt("zones", ctx->count);
            info.Real("period", ctx->hasPeriod ? double(ctx->period) : 0.0);
            info.Bool("calibrated", ctx->hasCalibration);
            info.UInt("overflow", ctx->overflow);
            info.EndObject();

            struct Frame { const tracy::GpuEvent* ev; int64_t parent; int depth; };
            std::vector<Frame> stack;
            for (const auto& [threadKey, threadData] : ctx->threadData) {
                (void)threadKey;
                auto pushChildren = [&](const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& vec,
                                        int64_t parent, int depth) {
                    const size_t base = stack.size();
                    ForEachEvent<tracy::GpuEvent>(vec, [&](const tracy::GpuEvent& ev) {
                        stack.push_back({&ev, parent, depth});
                    });
                    std::reverse(stack.begin() + base, stack.end());
                };
                pushChildren(threadData.timeline, -1, 0);
                while (!stack.empty()) {
                    const Frame f = stack.back();
                    stack.pop_back();
                    const tracy::GpuEvent& ev = *f.ev;
                    const int64_t id = int64_t(nextId++);
                    const auto& sl = w.GetSourceLocation(ev.SrcLoc());
                    const std::string name = SrcLocName(w, sl);
                    const bool resolved = ev.GpuStart() >= 0 && ev.GpuEnd() >= 0;
                    csv.Int(id).Int(f.parent).UInt(ci).UInt(w.DecompressThread(ev.Thread())).Int(f.depth)
                       .Int(ev.CpuStart()).Int(ev.CpuEnd());
                    if (resolved) csv.Int(ev.GpuStart()).Int(ev.GpuEnd()).Int(ev.GpuEnd() - ev.GpuStart());
                    else          csv.Empty().Empty().Empty();
                    csv.Str(name).Int(ev.SrcLoc()).UInt(ev.query_id).End();
                    ++counts.gpuZones;
                    if (resolved) stats[{name, ci}].Add(ev.GpuEnd() - ev.GpuStart());
                    else ++counts.unresolvedGpuZones;
                    if (ev.Child() >= 0) pushChildren(w.GetGpuChildren(ev.Child()), id, f.depth + 1);
                }
            }
        }
        info.EndArray();

        Csv st(dir / "gpu_stats.csv", {"name", "context", "count", "total_ns", "self_total_ns", "mean_ns", "min_ns",
                                      "p50_ns", "p90_ns", "p99_ns", "max_ns", "std_ns", "self_mean_ns", "self_max_ns"});
        for (auto& [key, s] : stats) WriteStatsRow(st, key.first, std::to_string(key.second), s);
    }

    void ExportPlots(Worker& w, const std::filesystem::path& dir, Json& info, Counts& counts) {
        Csv csv(dir / "plots.csv", {"plot", "time_ns", "value"});
        info.BeginArray("plots");
        for (const tracy::PlotData* p : w.GetPlots()) {
            const std::string name = PlotName(w, *p);
            for (const auto& item : p->data) csv.Str(name).Int(item.time.Val()).Real(item.val).End();
            counts.plotPoints += p->data.size();
            info.BeginObject();
            info.Str("name", name);
            info.Str("type", PlotTypeName(p->type));
            info.Str("format", PlotFormatName(p->format));
            info.UInt("count", p->data.size());
            if (!p->data.empty()) {
                info.Real("min", p->min);
                info.Real("max", p->max);
                info.Real("mean", p->sum / double(p->data.size()));
            }
            info.EndObject();
        }
        info.EndArray();
    }

    void ExportMessages(Worker& w, const std::filesystem::path& dir, Counts& counts) {
        Csv csv(dir / "messages.csv", {"time_ns", "thread", "source", "severity", "color", "text"});
        for (const auto& m : w.GetMessages()) {
            csv.Int(m->time).UInt(w.DecompressThread(m->thread))
               .Str(m->source == tracy::MessageSourceType::Tracy ? "tracy" : "user")
               .Str(SeverityName(m->severity)).Color(m->color).Str(w.GetString(m->ref)).End();
            ++counts.messages;
        }
    }

    void ExportLocks(Worker& w, const std::filesystem::path& dir, Counts& counts) {
        Csv summary(dir / "locks.csv", {"lock", "name", "srcloc", "type", "threads", "announce_ns", "terminate_ns",
                                        "contended", "acquisitions", "hold_total_ns", "hold_max_ns",
                                        "waits", "wait_total_ns", "wait_max_ns"});
        Csv events(dir / "lock_events.csv", {"lock", "time_ns", "thread", "event", "lock_count", "waiters"});
        std::vector<std::pair<uint32_t, const tracy::LockMap*>> locks;
        for (const auto& kv : w.GetLockMap()) locks.emplace_back(kv.first, kv.second);
        std::sort(locks.begin(), locks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [id, lock] : locks) {
            if (!lock->valid) continue;
            const auto& sl = w.GetSourceLocation(lock->srcloc);
            const std::string name = lock->customName.Active() ? w.GetString(lock->customName) : SrcLocName(w, sl);
            // Per lock-thread slot: when it started waiting / obtained.
            std::vector<int64_t> waitStart(lock->threadList.size(), -1), holdStart(lock->threadList.size(), -1);
            uint64_t acquisitions = 0, waits = 0;
            int64_t holdTotal = 0, holdMax = 0, waitTotal = 0, waitMax = 0;
            for (const auto& lp : lock->timeline) {
                const tracy::LockEvent& ev = *lp.ptr;
                const size_t slot = ev.thread;
                const uint64_t tid = slot < lock->threadList.size() ? lock->threadList[slot] : 0;
                const int64_t t = ev.Time();
                events.UInt(id).Int(t).UInt(tid).Str(LockEventName(ev.type)).UInt(lp.lockCount)
                      .Int(std::popcount(lp.waitList)).End();
                ++counts.lockEvents;
                if (slot >= waitStart.size()) continue;
                switch (ev.type) {
                    case tracy::LockEvent::Type::Wait:
                    case tracy::LockEvent::Type::WaitShared:
                        waitStart[slot] = t;
                        break;
                    case tracy::LockEvent::Type::Obtain:
                    case tracy::LockEvent::Type::ObtainShared:
                        ++acquisitions;
                        if (waitStart[slot] >= 0) {
                            const int64_t d = t - waitStart[slot];
                            ++waits; waitTotal += d; waitMax = std::max(waitMax, d);
                            waitStart[slot] = -1;
                        }
                        holdStart[slot] = t;
                        break;
                    case tracy::LockEvent::Type::Release:
                    case tracy::LockEvent::Type::ReleaseShared:
                        if (holdStart[slot] >= 0) {
                            const int64_t d = t - holdStart[slot];
                            holdTotal += d; holdMax = std::max(holdMax, d);
                            holdStart[slot] = -1;
                        }
                        break;
                }
            }
            summary.UInt(id).Str(name).Int(lock->srcloc)
                   .Str(lock->type == tracy::LockType::SharedLockable ? "shared" : "exclusive")
                   .UInt(lock->threadList.size()).Int(lock->timeAnnounce).Int(lock->timeTerminate)
                   .Int(lock->isContended ? 1 : 0).UInt(acquisitions).Int(holdTotal).Int(holdMax)
                   .UInt(waits).Int(waitTotal).Int(waitMax).End();
        }
    }

    void ExportMemory(Worker& w, const std::filesystem::path& dir, Json& info, bool writeRows, Counts& counts) {
        std::unique_ptr<Csv> csv;
        if (writeRows) {
            csv = std::make_unique<Csv>(dir / "memory.csv",
                std::initializer_list<const char*>{"pool", "ptr", "size", "alloc_ns", "free_ns", "alloc_thread", "free_thread"});
        }
        info.BeginArray("memory_pools");
        for (const auto& [nameKey, mem] : w.GetMemNameMap()) {
            const std::string pool = nameKey == 0 ? "default" : w.GetString(nameKey);
            if (csv) {
                for (const auto& ev : mem->data) {
                    csv->Str(pool).Hex(ev.Ptr()).UInt(ev.Size()).Int(ev.TimeAlloc());
                    if (ev.TimeFree() >= 0) csv->Int(ev.TimeFree()); else csv->Empty();
                    csv->UInt(w.DecompressThread(ev.ThreadAlloc()));
                    if (ev.TimeFree() >= 0) csv->UInt(w.DecompressThread(ev.ThreadFree())); else csv->Empty();
                    csv->End();
                }
            }
            counts.allocations += mem->data.size();
            info.BeginObject();
            info.Str("name", pool);
            info.UInt("allocations", mem->data.size());
            info.UInt("frees", mem->frees.size());
            info.UInt("active", mem->active.size());
            info.UInt("usage_bytes", mem->usage);
            if (!mem->data.empty()) {
                info.UInt("low_address", mem->low);
                info.UInt("high_address", mem->high);
            }
            info.EndObject();
        }
        info.EndArray();
    }

    void ExportContextSwitches(Worker& w, const std::filesystem::path& dir, Counts& counts) {
        Csv csv(dir / "context_switches.csv", {"thread", "wakeup_ns", "start_ns", "end_ns", "running_ns", "cpu",
                                               "wakeup_cpu", "reason", "state"});
        for (const tracy::ThreadData* td : w.GetThreadData()) {
            const tracy::ContextSwitch* cs = w.GetContextSwitchData(td->id);
            if (!cs) continue;
            for (const auto& s : cs->v) {
                csv.UInt(td->id).Int(s.WakeupVal()).Int(s.Start());
                if (s.IsEndValid()) csv.Int(s.End()).Int(s.End() - s.Start()); else csv.Empty().Empty();
                csv.UInt(s.Cpu()).UInt(s.WakeupCpu()).Int(int(s.Reason())).Int(s.State()).End();
                ++counts.contextSwitches;
            }
        }
    }

    // Root→leaf frame names of a callstack, inline frames included.
    void StackFrames(Worker& w, uint32_t callstack, std::vector<std::string>& out,
                     std::string* leafFile, uint32_t* leafLine) {
        out.clear();
        if (callstack == 0) return;
        const auto& cs = w.GetCallstack(callstack);
        for (int i = int(cs.size()) - 1; i >= 0; --i) {
            const tracy::CallstackFrameData* fd = w.GetCallstackFrame(cs[i]);
            if (!fd) { out.emplace_back("[unresolved]"); continue; }
            for (int j = int(fd->size) - 1; j >= 0; --j) {
                const auto& frame = fd->data[j];
                out.emplace_back(w.GetString(frame.name));
                if (i == 0 && j == 0) {
                    if (leafFile) *leafFile = w.GetString(frame.file);
                    if (leafLine) *leafLine = frame.line;
                }
            }
        }
    }

    void ExportSamples(Worker& w, const std::filesystem::path& dir, bool writeRows, Counts& counts) {
        std::unique_ptr<Csv> csv;
        if (writeRows) {
            csv = std::make_unique<Csv>(dir / "samples.csv",
                std::initializer_list<const char*>{"time_ns", "thread", "leaf", "leaf_file", "leaf_line", "stack"});
        }
        std::map<std::string, uint64_t> folded;
        std::vector<std::string> frames;
        for (const tracy::ThreadData* td : w.GetThreadData()) {
            if (td->samples.empty()) continue;
            const std::string threadName = w.GetThreadName(td->id);
            for (const auto& sample : td->samples) {
                std::string leafFile;
                uint32_t leafLine = 0;
                StackFrames(w, sample.callstack.Val(), frames, &leafFile, &leafLine);
                std::string stack;
                for (const auto& f : frames) {
                    if (!stack.empty()) stack += ';';
                    stack += f;
                }
                if (csv) {
                    csv->Int(sample.time.Val()).UInt(td->id).Str(frames.empty() ? "" : frames.back())
                        .Str(leafFile).UInt(leafLine).Str(stack).End();
                }
                ++folded[threadName + (stack.empty() ? "" : ";" + stack)];
                ++counts.samples;
            }
        }
        if (!folded.empty()) {
            FILE* f = std::fopen((dir / "samples_folded.txt").string().c_str(), "wb");
            if (f) {
                for (const auto& [stack, n] : folded) std::fprintf(f, "%s %" PRIu64 "\n", stack.c_str(), n);
                std::fclose(f);
            }
        }
    }

    void WriteInfoHeader(Worker& w, Json& info, const Options& opt) {
        info.Str("exporter", "tracy-export");
        info.Str("tracy_version", std::to_string(tracy::Version::Major) + "." + std::to_string(tracy::Version::Minor) +
                                  "." + std::to_string(tracy::Version::Patch));
        const int tv = w.GetTraceVersion();
        info.Str("trace_file_version", std::to_string(tv >> 16) + "." + std::to_string((tv >> 8) & 0xFF) + "." +
                                       std::to_string(tv & 0xFF));
        info.Str("trace_path", std::filesystem::absolute(opt.trace).string());
        info.Str("capture_name", w.GetCaptureName());
        info.Str("capture_program", w.GetCaptureProgram());
        info.UInt("capture_time_unix", w.GetCaptureTime());
        info.UInt("executable_time_unix", w.GetExecutableTime());
        info.Str("host_info", w.GetHostInfo());
        info.Str("cpu_manufacturer", w.GetCpuManufacturer());
        info.UInt("cpu_id", w.GetCpuId());
        info.UInt("pid", w.GetPid());
        info.Int("resolution_ns", w.GetResolution());
        info.Int("sampling_period_ns", w.GetSamplingPeriod());
        info.Int("first_time_ns", w.GetFirstTime());
        info.Int("last_time_ns", w.GetLastTime());
        info.Int("load_time_ns", w.GetLoadTime());
        info.Bool("excessive_zone_depth", w.HasExcessiveZoneDepth());
        if (w.GetFailureType() != Worker::Failure::None) info.Str("failure", Worker::GetFailureString(w.GetFailureType()));

        info.BeginArray("app_info");
        for (const auto& s : w.GetAppInfo()) { info.Str(nullptr, w.GetString(s)); }
        info.EndArray();

        info.BeginArray("parameters");
        for (const auto& p : w.GetParameters()) {
            info.BeginObject();
            info.Str("name", w.GetString(p.name));
            info.Int("value", p.val);
            info.EndObject();
        }
        info.EndArray();

        const auto& crash = w.GetCrashEvent();
        if (crash.thread != 0) {
            info.BeginObject("crash");
            info.UInt("thread", crash.thread);
            info.Str("thread_name", w.GetThreadName(crash.thread));
            info.Int("time_ns", crash.time);
            info.Str("message", crash.message ? w.GetString(crash.message) : "");
            std::vector<std::string> frames;
            StackFrames(w, crash.callstack, frames, nullptr, nullptr);
            info.BeginArray("callstack");
            for (const auto& f : frames) info.Str(nullptr, f);
            info.EndArray();
            info.EndObject();
        }

        info.BeginArray("threads");
        for (const tracy::ThreadData* td : w.GetThreadData()) {
            info.BeginObject();
            info.UInt("tid", td->id);
            info.Str("name", w.GetThreadName(td->id));
            info.UInt("zones", td->count);
            info.EndObject();
        }
        info.EndArray();
    }

    void WriteCounts(Json& info, const Counts& c, Worker& w) {
        info.BeginObject("counts");
        info.UInt("threads", w.GetThreadData().size());
        info.UInt("zones", c.zones);
        info.UInt("unfinished_zones", c.unfinishedZones);
        info.UInt("gpu_zones", c.gpuZones);
        info.UInt("unresolved_gpu_zones", c.unresolvedGpuZones);
        info.UInt("frames", c.frames);
        info.UInt("plot_points", c.plotPoints);
        info.UInt("messages", c.messages);
        info.UInt("locks", w.GetLockMap().size());
        info.UInt("lock_events", c.lockEvents);
        info.UInt("allocations", c.allocations);
        info.UInt("context_switches", c.contextSwitches);
        info.UInt("samples", c.samples);
        info.UInt("frame_images", w.GetFrameImageCount());
        info.UInt("source_locations", w.GetSrcLocCount());
        info.EndObject();
    }

    void PrintUsage() {
        std::fprintf(stderr,
            "tracy-export %d.%d.%d — dump a Tracy capture into CSV tables + info.json\n\n"
            "usage: tracy-export <capture.tracy> -o <out_dir> [--no-zones] [--no-memory] [--no-samples]\n"
            "       tracy-export -V\n\n"
            "  --no-zones    skip the raw zones/ tables (zone_stats.csv and frame_zones.csv are still written)\n"
            "  --no-memory   skip memory.csv (pool summaries stay in info.json)\n"
            "  --no-samples  skip samples.csv (samples_folded.txt is still written)\n",
            tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch);
    }

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-V" || a == "--version") {
            std::printf("tracy-export %d.%d.%d\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch);
            return 0;
        }
        if (a == "-h" || a == "--help") { PrintUsage(); return 0; }
        if ((a == "-o" || a == "--out") && i + 1 < argc) { opt.out = argv[++i]; continue; }
        if (a == "--no-zones") { opt.zones = false; continue; }
        if (a == "--no-memory") { opt.memory = false; continue; }
        if (a == "--no-samples") { opt.samples = false; continue; }
        if (!a.empty() && a[0] == '-') { PrintUsage(); return 1; }
        opt.trace = argv[i];
    }
    if (opt.trace.empty() || opt.out.empty()) { PrintUsage(); return 1; }

    std::unique_ptr<tracy::FileRead> file;
    try {
        file.reset(tracy::FileRead::Open(opt.trace.c_str()));
    } catch (const tracy::NotTracyDump&) {
        std::fprintf(stderr, "tracy-export: %s is not a Tracy capture\n", opt.trace.c_str());
        return 4;
    } catch (const tracy::FileReadError&) {
        std::fprintf(stderr, "tracy-export: read error on %s\n", opt.trace.c_str());
        return 4;
    }
    if (!file) {
        std::fprintf(stderr, "tracy-export: cannot open %s\n", opt.trace.c_str());
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Worker> worker;
    try {
        worker = std::make_unique<Worker>(*file);
    } catch (const tracy::UnsupportedVersion& e) {
        std::fprintf(stderr,
            "tracy-export: %s was saved by Tracy %d.%d.%d, newer than this exporter (%d.%d.%d).\n"
            "Bump the tracy GIT_TAG in CMakeLists.txt to the viewer's version and run tools/build_tracy_tools.sh.\n",
            opt.trace.c_str(), e.version >> 16, (e.version >> 8) & 0xFF, e.version & 0xFF,
            tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch);
        return 2;
    } catch (const tracy::LegacyVersion& e) {
        std::fprintf(stderr,
            "tracy-export: %s is a legacy capture (Tracy %d.%d.%d); convert it with tools/tracy/tracy-update first.\n",
            opt.trace.c_str(), e.version >> 16, (e.version >> 8) & 0xFF, e.version & 0xFF);
        return 3;
    } catch (const tracy::FileReadError&) {
        std::fprintf(stderr, "tracy-export: %s is truncated or corrupt\n", opt.trace.c_str());
        return 4;
    }
    file.reset();
    // Statistics, memory plots and CPU usage are rebuilt on background threads.
    while (!worker->IsBackgroundDone()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Worker& w = *worker;

    std::error_code ec;
    std::filesystem::create_directories(opt.out, ec);
    if (ec) {
        std::fprintf(stderr, "tracy-export: cannot create %s: %s\n", opt.out.string().c_str(), ec.message().c_str());
        return 1;
    }

    Counts counts;
    {
        Json info(opt.out / "info.json");
        info.BeginObject();
        WriteInfoHeader(w, info, opt);
        const std::vector<FrameSpans> frameSets = CollectFrameSpans(w);
        ExportSourceLocations(w, opt.out);
        ExportZones(w, opt.out, opt.zones, frameSets, counts);
        ExportFrames(w, opt.out, frameSets, info, counts);
        ExportGpu(w, opt.out, info, counts);
        ExportPlots(w, opt.out, info, counts);
        ExportMessages(w, opt.out, counts);
        ExportLocks(w, opt.out, counts);
        ExportMemory(w, opt.out, info, opt.memory, counts);
        ExportContextSwitches(w, opt.out, counts);
        ExportSamples(w, opt.out, opt.samples, counts);
        WriteCounts(info, counts, w);
        info.Real("export_seconds",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        info.EndObject();
    }
    std::fprintf(stderr, "tracy-export: %" PRIu64 " zones, %" PRIu64 " frames, %" PRIu64 " GPU zones, %" PRIu64
                 " plot points, %" PRIu64 " messages -> %s\n",
                 counts.zones, counts.frames, counts.gpuZones, counts.plotPoints, counts.messages,
                 opt.out.string().c_str());
    return 0;
}
