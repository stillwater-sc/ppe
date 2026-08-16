// schedule_report.hpp -- schedule and occupancy, as a self-contained page.
//
// The fifth pipeline stage: detect, model, measure, trace, VISUALIZE. Two views
// of the same spans, because they answer different questions:
//
//   SCHEDULE -- which work ran on which lane, and when. One row per thread, one
//   bar per span. This is what a trace viewer already shows, and it answers
//   "what happened".
//
//   OCCUPANCY -- how much of the machine was busy at each instant, as a
//   fraction of lanes inside a span. This is what a trace viewer does NOT show,
//   and it answers "what did the machine have left". THE IDLE IS THE POINT: a
//   schedule that looks dense can still leave half the fabric unused, and the
//   gap between the two is where the performance went.
//
// WHY NOT JUST USE PERFETTO. PPE already exports Chrome Trace JSON and Perfetto
// renders it well; that remains the right tool for exploring a trace. This page
// exists for the aggregate view Perfetto has no notion of, for embedding in a
// study next to the numbers that produced it, and for being readable with no
// network and no application -- the same reason the topology report is a file
// rather than a service.
//
// HONEST LIMITS, stated because a picture invites more trust than a table:
//
//   Occupancy is computed over LANES THAT RECORDED SOMETHING, not over the
//   machine's cores. A run that traced two threads on a twelve-core part shows
//   100% occupancy when both are busy. The lane count is printed beside the
//   chart so the denominator is never implicit.
//
//   A lane that registered a thread name but never recorded a span is EXCLUDED
//   from that denominator. It carries no information about occupancy, and
//   counting it silently deflates every figure: the first run of this against a
//   threaded sweep reported 6% peak occupancy, because fifteen worker threads
//   had named themselves and only the main thread had instrumented anything.
//   6% was arithmetically correct and told the reader nothing true.
//
//   Spans are what someone chose to instrument. Time in uninstrumented code is
//   invisible here and reads as idle. The sampling profiler is the instrument
//   for that question; this one describes the spans it was given.
//
//   A trace that dropped events is drawn with holes. The drop count is carried
//   from the recorder and shown, because a truncated schedule looks exactly like
//   a sparse one.
#pragma once

#include <ppe/provenance.hpp>
#include <ppe/trace.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ppe::report {

struct schedule_stats {
    std::uint64_t span_count = 0;
    std::uint64_t dropped = 0;
    std::size_t   lanes = 0;        ///< lanes that recorded at least one span
    std::size_t   empty_lanes = 0;  ///< registered a name, recorded nothing
    std::uint64_t span_ns = 0;      ///< first span start
    std::uint64_t end_ns = 0;       ///< last span end
    double peak_occupancy = 0.0;    ///< busiest bucket, as a fraction of lanes
    double mean_occupancy = 0.0;
};

namespace detail {

/// A stable colour per span name, so the same work is the same colour across
/// lanes and across runs. Hue only -- saturation and lightness are fixed so no
/// span can come out invisible against either theme.
inline std::string colour_for(const std::string& name) {
    std::uint32_t h = 2166136261u;
    for (const char c : name) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    const unsigned hue = h % 360u;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "hsl(%u 62%% 52%%)", hue);
    return std::string(buf);
}

inline std::string time_label(double ns) {
    char buf[32];
    if (ns >= 1e9) std::snprintf(buf, sizeof(buf), "%.3g s", ns / 1e9);
    else if (ns >= 1e6) std::snprintf(buf, sizeof(buf), "%.3g ms", ns / 1e6);
    else if (ns >= 1e3) std::snprintf(buf, sizeof(buf), "%.3g us", ns / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.3g ns", ns);
    return std::string(buf);
}

inline std::string esc(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace detail

/// Render the schedule and occupancy of a trace snapshot.
inline std::string schedule_to_html(const std::vector<trace::recorder::lane>& lanes,
                                    const provenance& prov,
                                    schedule_stats* out_stats = nullptr) {
    schedule_stats st;

    // Only lanes that recorded something take part; see the header note.
    std::vector<trace::recorder::lane> active;
    for (const auto& l : lanes) {
        st.dropped += l.dropped;
        if (!l.events.empty()) active.push_back(l);
        else ++st.empty_lanes;
    }
    st.lanes = active.size();

    std::uint64_t t0 = UINT64_MAX;
    std::uint64_t t1 = 0;
    for (const auto& l : active) {
        for (const trace::event& e : l.events) {
            ++st.span_count;
            t0 = std::min(t0, e.start_ns);
            t1 = std::max(t1, e.start_ns + e.duration_ns);
        }
    }
    if (st.span_count == 0) {
        t0 = 0;
        t1 = 1;
    }
    st.span_ns = t0;
    st.end_ns = t1;
    const double span = static_cast<double>(t1 - t0);

    // Occupancy: bucket the timeline and count lanes with a span covering each
    // bucket's midpoint. Midpoint sampling rather than overlap-area keeps this
    // O(spans) per bucket and cannot report more than the lane count.
    constexpr int kBuckets = 240;
    std::vector<double> occ(kBuckets, 0.0);
    if (st.span_count > 0 && !active.empty()) {
        for (int b = 0; b < kBuckets; ++b) {
            const double mid = t0 + span * (b + 0.5) / kBuckets;
            int busy = 0;
            for (const auto& l : active) {
                for (const trace::event& e : l.events) {
                    if (mid >= static_cast<double>(e.start_ns) &&
                        mid < static_cast<double>(e.start_ns + e.duration_ns)) {
                        ++busy;
                        break;   // one lane counts once, however deeply nested
                    }
                }
            }
            occ[b] = static_cast<double>(busy) / static_cast<double>(active.size());
            st.peak_occupancy = std::max(st.peak_occupancy, occ[b]);
            st.mean_occupancy += occ[b] / kBuckets;
        }
    }
    if (out_stats != nullptr) *out_stats = st;

    // -- page -------------------------------------------------------------
    const double width = 960.0;
    const double lane_h = 26.0;
    const double chart_h = 120.0;
    const double left = 120.0;
    const double plot_w = width - left - 20.0;
    const double sched_h = active.empty() ? lane_h : active.size() * lane_h;

    std::string h;
    h += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    h += "<title>PPE schedule -- " + detail::esc(prov.cpu.name) + "</title>\n";
    h += "<style>\n"
         ":root{--bg:#fbfbfa;--fg:#1a1a19;--mut:#6b6b68;--line:#dcdcd8;--card:#fff;"
         "--accent:#1f7a58;--warn:#8a5a00;--grid:#ececea}\n"
         "@media(prefers-color-scheme:dark){:root{--bg:#16171a;--fg:#e8e8e6;"
         "--mut:#9a9a97;--line:#2e3034;--card:#1d1f23;--accent:#2fa37a;"
         "--warn:#d1a04a;--grid:#26282c}}\n"
         "*{box-sizing:border-box}body{margin:0;padding:2rem 1.25rem;background:var(--bg);"
         "color:var(--fg);font:15px/1.55 ui-sans-serif,system-ui,-apple-system,"
         "'Segoe UI',Roboto,sans-serif}\n"
         ".wrap{max-width:64rem;margin:0 auto}h1{font-size:1.4rem;margin:0 0 .25rem}\n"
         ".sub{color:var(--mut);margin:0 0 1.25rem;font-size:.9rem}\n"
         ".facts{display:flex;flex-wrap:wrap;gap:.5rem 1.5rem;margin:0 0 1.25rem;"
         "padding:.75rem 1rem;border:1px solid var(--line);border-radius:8px;"
         "background:var(--card)}\n"
         ".fact b{display:block;font-size:.72rem;text-transform:uppercase;"
         "letter-spacing:.05em;color:var(--mut);font-weight:600}\n"
         ".panel{border:1px solid var(--line);border-radius:10px;background:var(--card);"
         "padding:1rem;margin-bottom:1.25rem;overflow-x:auto}\n"
         ".panel h2{font-size:1rem;margin:0 0 .15rem}\n"
         ".panel p{margin:0 0 .75rem;color:var(--mut);font-size:.85rem}\n"
         "text{fill:var(--mut);font:11px ui-sans-serif,system-ui,sans-serif}\n"
         ".lane-label{font-size:11px}\n"
         ".note{padding:.75rem 1rem;border-left:3px solid var(--warn);"
         "background:var(--card);font-size:.86rem;margin-bottom:1.25rem}\n"
         "footer{margin-top:1.5rem;color:var(--mut);font-size:.78rem;"
         "border-top:1px solid var(--line);padding-top:.75rem}\n"
         "code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}\n"
         "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    h += "<h1>Schedule and occupancy</h1>\n";
    h += "<p class=\"sub\">" + detail::esc(prov.cpu.name) + "</p>\n";

    h += "<div class=\"facts\">\n";
    auto fact = [&](const char* k, const std::string& v) {
        h += "<div class=\"fact\"><b>" + std::string(k) + "</b>" + detail::esc(v) +
             "</div>\n";
    };
    fact("lanes", std::to_string(st.lanes));
    if (st.empty_lanes > 0) {
        fact("uninstrumented lanes", std::to_string(st.empty_lanes));
    }
    fact("spans", std::to_string(st.span_count));
    fact("wall time", detail::time_label(span));
    char pct[32];
    std::snprintf(pct, sizeof(pct), "%.0f%%", st.peak_occupancy * 100.0);
    fact("peak occupancy", pct);
    std::snprintf(pct, sizeof(pct), "%.0f%%", st.mean_occupancy * 100.0);
    fact("mean occupancy", pct);
    h += "</div>\n";

    if (st.dropped > 0) {
        h += "<div class=\"note\"><b>" + std::to_string(st.dropped) +
             " events were dropped.</b> The schedule below has holes, and a truncated "
             "schedule looks exactly like a sparse one. Raise the recorder's capacity "
             "before reading anything into the gaps.</div>\n";
    }

    // -- schedule ----------------------------------------------------------
    h += "<div class=\"panel\">\n<h2>Schedule</h2>\n";
    h += "<p>One row per thread that recorded a span. Colour identifies the span "
         "name.</p>\n";
    h += "<svg width=\"" + std::to_string(static_cast<int>(width)) + "\" height=\"" +
         std::to_string(static_cast<int>(sched_h + 28)) + "\" role=\"img\">\n";

    for (std::size_t i = 0; i < active.size(); ++i) {
        const auto& l = active[i];
        const double y = i * lane_h;
        h += "<rect x=\"" + std::to_string(left) + "\" y=\"" + std::to_string(y + 3) +
             "\" width=\"" + std::to_string(plot_w) + "\" height=\"" +
             std::to_string(lane_h - 6) + "\" fill=\"var(--grid)\"/>\n";
        const std::string label =
            l.name.empty() ? ("tid " + std::to_string(l.tid)) : l.name;
        h += "<text class=\"lane-label\" x=\"" + std::to_string(left - 8) + "\" y=\"" +
             std::to_string(y + lane_h / 2 + 4) + "\" text-anchor=\"end\">" +
             detail::esc(label) + "</text>\n";

        for (const trace::event& e : l.events) {
            const double x = left + plot_w * (static_cast<double>(e.start_ns - t0)) /
                                        (span > 0 ? span : 1);
            double w = plot_w * static_cast<double>(e.duration_ns) /
                       (span > 0 ? span : 1);
            // A span shorter than a pixel still happened; drawing it at zero
            // width would erase exactly the short work a schedule is read for.
            if (w < 1.0) w = 1.0;
            const std::string name = e.name ? e.name : "span";
            h += "<rect x=\"" + std::to_string(x) + "\" y=\"" +
                 std::to_string(y + 4) + "\" width=\"" + std::to_string(w) +
                 "\" height=\"" + std::to_string(lane_h - 8) + "\" fill=\"" +
                 detail::colour_for(name) + "\" rx=\"2\"><title>" + detail::esc(name) +
                 " -- " + detail::time_label(static_cast<double>(e.duration_ns)) +
                 "</title></rect>\n";
        }
    }
    h += "<text x=\"" + std::to_string(left) + "\" y=\"" +
         std::to_string(sched_h + 18) + "\">0</text>\n";
    h += "<text x=\"" + std::to_string(left + plot_w) + "\" y=\"" +
         std::to_string(sched_h + 18) + "\" text-anchor=\"end\">" +
         detail::time_label(span) + "</text>\n";
    h += "</svg>\n</div>\n";

    // -- occupancy ---------------------------------------------------------
    h += "<div class=\"panel\">\n<h2>Occupancy</h2>\n";
    h += "<p>Fraction of the " + std::to_string(st.lanes) +
         " instrumented lane(s) inside a span at each instant. This is a fraction of "
         "<em>traced lanes</em>, not of the machine's cores &mdash; the idle here is "
         "idle among the threads that recorded, and time in uninstrumented code reads "
         "as idle.</p>\n";
    h += "<svg width=\"" + std::to_string(static_cast<int>(width)) + "\" height=\"" +
         std::to_string(static_cast<int>(chart_h + 30)) + "\" role=\"img\">\n";
    for (int g = 0; g <= 4; ++g) {
        const double y = chart_h * g / 4.0 + 6;
        h += "<line x1=\"" + std::to_string(left) + "\" y1=\"" + std::to_string(y) +
             "\" x2=\"" + std::to_string(left + plot_w) + "\" y2=\"" +
             std::to_string(y) + "\" stroke=\"var(--grid)\"/>\n";
        h += "<text x=\"" + std::to_string(left - 8) + "\" y=\"" +
             std::to_string(y + 4) + "\" text-anchor=\"end\">" +
             std::to_string(100 - g * 25) + "%</text>\n";
    }
    std::string path = "M " + std::to_string(left) + " " + std::to_string(chart_h + 6);
    for (int b = 0; b < kBuckets; ++b) {
        const double x = left + plot_w * (b + 0.5) / kBuckets;
        const double y = 6 + chart_h * (1.0 - occ[b]);
        path += " L " + std::to_string(x) + " " + std::to_string(y);
    }
    path += " L " + std::to_string(left + plot_w) + " " + std::to_string(chart_h + 6) +
            " Z";
    h += "<path d=\"" + path + "\" fill=\"var(--accent)\" fill-opacity=\"0.35\" "
         "stroke=\"var(--accent)\" stroke-width=\"1.5\"/>\n";
    h += "</svg>\n</div>\n";

    h += "<footer>PPE " + detail::esc(prov.ppe_version) + " &middot; commit " +
         detail::esc(prov.git_commit);
    if (prov.git_dirty == "1") h += " (dirty)";
    h += " &middot; " + detail::esc(prov.compiler) + " &middot; build ISA " +
         detail::esc(prov.isa) + " &middot; " + detail::esc(prov.utc_timestamp) +
         "</footer>\n</div>\n</body>\n</html>\n";
    return h;
}

}  // namespace ppe::report
